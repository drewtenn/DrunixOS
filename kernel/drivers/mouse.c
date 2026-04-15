/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mouse.c — PS/2 mouse packet decoding and IRQ12 input dispatch.
 */

#include "mouse.h"
#include "irq.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL   0x02

#define PS2_RESP_ACK     0xFA
#define PS2_RESP_BAT     0xAA
#define PS2_RESP_RESEND   0xFE

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);

static mouse_packet_stream_t g_stream;
static int g_pointer_x = 40;
static int g_pointer_y = 12;

static int ps2_wait_input_ready(void)
{
    for (int i = 0; i < 100000; i++) {
        if ((port_byte_in(PS2_STATUS_PORT) & PS2_STATUS_IN_FULL) == 0)
            return 0;
    }
    return -1;
}

static int ps2_wait_output_ready(void)
{
    for (int i = 0; i < 100000; i++) {
        if (port_byte_in(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL)
            return 0;
    }
    return -1;
}

static void ps2_flush_output(void)
{
    while (port_byte_in(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL)
        (void)port_byte_in(PS2_DATA_PORT);
}

static int ps2_write_command(uint8_t value)
{
    if (ps2_wait_input_ready() != 0)
        return -1;
    port_byte_out(PS2_CMD_PORT, value);
    return 0;
}

static int ps2_write_data(uint8_t value)
{
    if (ps2_wait_input_ready() != 0)
        return -1;
    port_byte_out(PS2_DATA_PORT, value);
    return 0;
}

static int ps2_read_data(uint8_t *out)
{
    if (!out || ps2_wait_output_ready() != 0)
        return -1;
    *out = port_byte_in(PS2_DATA_PORT);
    return 0;
}

static int ps2_read_controller_config(uint8_t *out)
{
    if (ps2_write_command(0x20) != 0)
        return -11;
    if (ps2_read_data(out) != 0)
        return -12;
    return 0;
}

static int ps2_write_controller_config(uint8_t value)
{
    if (ps2_write_command(0x60) != 0)
        return -21;
    if (ps2_write_data(value) != 0)
        return -22;
    return 0;
}

static int ps2_mouse_write(uint8_t value)
{
    uint8_t ack;

    if (ps2_write_command(0xD4) != 0)
        return -1;
    if (ps2_write_data(value) != 0)
        return -1;
    if (ps2_read_data(&ack) != 0)
        return -1;
    return ack == PS2_RESP_ACK ? 0 : -1;
}

static void pic_unmask_irq(uint8_t irq_num)
{
    uint16_t port = (irq_num < 8) ? 0x21 : 0xA1;
    uint8_t mask = port_byte_in(port);
    mask &= (uint8_t)~(1u << (irq_num & 7u));
    port_byte_out(port, mask);
}

int mouse_decode_packet(const mouse_packet_t *packet, desktop_pointer_event_t *ev)
{
    if (!packet || !ev)
        return -1;

    ev->dx = packet->dx;
    ev->dy = packet->dy;
    ev->left_down = (packet->buttons & 0x1u) != 0;
    return 0;
}

void mouse_stream_reset(mouse_packet_stream_t *stream)
{
    if (!stream)
        return;
    stream->bytes[0] = 0;
    stream->bytes[1] = 0;
    stream->bytes[2] = 0;
    stream->index = 0;
}

static int mouse_stream_is_response(uint8_t data)
{
    return data == PS2_RESP_ACK ||
           data == PS2_RESP_BAT ||
           data == PS2_RESP_RESEND;
}

int mouse_stream_consume(mouse_packet_stream_t *stream, uint8_t data,
                         mouse_packet_t *packet_out)
{
    if (!stream || !packet_out)
        return -1;

    if (stream->index == 0 && mouse_stream_is_response(data)) {
        mouse_stream_reset(stream);
        return 0;
    }

    if (stream->index == 0) {
        if ((data & 0x08u) == 0)
            return 0;
        if ((data & 0xC0u) != 0) {
            mouse_stream_reset(stream);
            return 0;
        }
    }

    stream->bytes[stream->index++] = data;
    if (stream->index < 3)
        return 0;

    packet_out->buttons = stream->bytes[0];
    packet_out->dx = (int8_t)stream->bytes[1];
    packet_out->dy = (int8_t)stream->bytes[2];
    mouse_stream_reset(stream);
    return 1;
}

static void mouse_update_pointer(desktop_state_t *desktop, desktop_pointer_event_t *ev)
{
    int max_x = 79;
    int max_y = 24;

    if (desktop && desktop->display) {
        max_x = desktop->display->cols - 1;
        max_y = desktop->display->rows - 1;
    }

    g_pointer_x += ev->dx;
    g_pointer_y -= ev->dy;

    if (g_pointer_x < 0)
        g_pointer_x = 0;
    if (g_pointer_y < 0)
        g_pointer_y = 0;
    if (g_pointer_x > max_x)
        g_pointer_x = max_x;
    if (g_pointer_y > max_y)
        g_pointer_y = max_y;

    ev->x = g_pointer_x;
    ev->y = g_pointer_y;
}

static void mouse_handler(void)
{
    desktop_pointer_event_t ev;
    mouse_packet_t packet;
    desktop_state_t *desktop;
    uint8_t data;

    data = port_byte_in(PS2_DATA_PORT);
    if (mouse_stream_consume(&g_stream, data, &packet) <= 0)
        return;
    if (mouse_decode_packet(&packet, &ev) != 0)
        return;

    desktop = desktop_global();
    mouse_update_pointer(desktop, &ev);

    if (desktop_is_active())
        desktop_handle_pointer(desktop, &ev);
}

int mouse_init(void)
{
    uint8_t config;
    int rc;

    mouse_stream_reset(&g_stream);
    irq_register(12, mouse_handler);
    pic_unmask_irq(2);
    pic_unmask_irq(12);

    if (ps2_write_command(0xA7) != 0)
        return -1;
    ps2_flush_output();

    rc = ps2_read_controller_config(&config);
    if (rc != 0)
        return rc;
    config |= 0x02u;
    rc = ps2_write_controller_config(config);
    if (rc != 0)
        return rc;

    if (ps2_write_command(0xA8) != 0)
        return -4;
    if (ps2_mouse_write(0xF4) != 0)
        return -5;
    return 0;
}

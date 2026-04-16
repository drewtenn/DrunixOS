#include "mouse.h"
#include "irq.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL   0x02

#define PS2_RESP_ACK   0xFA
#define PS2_RESP_BAT   0xAA
#define PS2_RESP_RESEND 0xFE
#ifndef MOUSE_FRAMEBUFFER_PIXEL_SCALE
#define MOUSE_FRAMEBUFFER_PIXEL_SCALE 4
#endif

#define MOUSE_FRAMEBUFFER_PIXEL_SCALE_MIN 1
#define MOUSE_FRAMEBUFFER_PIXEL_SCALE_MAX 16

#if MOUSE_FRAMEBUFFER_PIXEL_SCALE < MOUSE_FRAMEBUFFER_PIXEL_SCALE_MIN
#define MOUSE_EFFECTIVE_FRAMEBUFFER_PIXEL_SCALE MOUSE_FRAMEBUFFER_PIXEL_SCALE_MIN
#elif MOUSE_FRAMEBUFFER_PIXEL_SCALE > MOUSE_FRAMEBUFFER_PIXEL_SCALE_MAX
#define MOUSE_EFFECTIVE_FRAMEBUFFER_PIXEL_SCALE MOUSE_FRAMEBUFFER_PIXEL_SCALE_MAX
#else
#define MOUSE_EFFECTIVE_FRAMEBUFFER_PIXEL_SCALE MOUSE_FRAMEBUFFER_PIXEL_SCALE
#endif

#define MOUSE_PACKET_ALWAYS_ONE 0x08u
#define MOUSE_PACKET_X_SIGN     0x10u
#define MOUSE_PACKET_Y_SIGN     0x20u
#define MOUSE_PACKET_X_OVERFLOW 0x40u
#define MOUSE_PACKET_Y_OVERFLOW 0x80u

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);

static mouse_packet_stream_t g_stream;
static int g_pointer_pixel_x = 40 * (int)GUI_FONT_W;
static int g_pointer_pixel_y = 12 * (int)GUI_FONT_H;

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

int mouse_decode_packet(const mouse_packet_t *packet, desktop_pointer_event_t *ev)
{
    if (!packet || !ev)
        return -1;

    ev->dx = (packet->buttons & MOUSE_PACKET_X_OVERFLOW)
        ? ((packet->buttons & MOUSE_PACKET_X_SIGN) ? -127 : 127)
        : packet->dx;
    ev->dy = (packet->buttons & MOUSE_PACKET_Y_OVERFLOW)
        ? ((packet->buttons & MOUSE_PACKET_Y_SIGN) ? -127 : 127)
        : packet->dy;
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

int mouse_stream_consume(mouse_packet_stream_t *stream, uint8_t data,
                         mouse_packet_t *packet_out)
{
    if (!stream || !packet_out)
        return -1;

    if (stream->index == 0) {
        if ((data & MOUSE_PACKET_ALWAYS_ONE) == 0)
            return 0;
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

static int mouse_motion_scale(const desktop_state_t *desktop)
{
    if (desktop && desktop->framebuffer_enabled && desktop->framebuffer)
        return MOUSE_EFFECTIVE_FRAMEBUFFER_PIXEL_SCALE;
    return 1;
}

static void mouse_update_pointer(desktop_state_t *desktop, desktop_pointer_event_t *ev)
{
    int max_x = 80 * (int)GUI_FONT_W - 1;
    int max_y = 25 * (int)GUI_FONT_H - 1;
    int scale = mouse_motion_scale(desktop);

    if (desktop && desktop->framebuffer_enabled && desktop->framebuffer) {
        if (desktop->framebuffer->width > 0)
            max_x = (int)desktop->framebuffer->width - 1;
        if (desktop->framebuffer->height > 0)
            max_y = (int)desktop->framebuffer->height - 1;
    } else if (desktop && desktop->display) {
        if (desktop->display->cols > 0)
            max_x = desktop->display->cols * (int)GUI_FONT_W - 1;
        if (desktop->display->rows > 0)
            max_y = desktop->display->rows * (int)GUI_FONT_H - 1;
    }

    g_pointer_pixel_x += ev->dx * scale;
    g_pointer_pixel_y -= ev->dy * scale;

    if (g_pointer_pixel_x < 0)
        g_pointer_pixel_x = 0;
    if (g_pointer_pixel_y < 0)
        g_pointer_pixel_y = 0;
    if (g_pointer_pixel_x > max_x)
        g_pointer_pixel_x = max_x;
    if (g_pointer_pixel_y > max_y)
        g_pointer_pixel_y = max_y;

    ev->pixel_x = g_pointer_pixel_x;
    ev->pixel_y = g_pointer_pixel_y;
    ev->x = g_pointer_pixel_x / (int)GUI_FONT_W;
    ev->y = g_pointer_pixel_y / (int)GUI_FONT_H;
}

#ifdef KTEST_ENABLED
void mouse_pointer_reset_for_test(int pixel_x, int pixel_y)
{
    g_pointer_pixel_x = pixel_x;
    g_pointer_pixel_y = pixel_y;
}

void mouse_update_pointer_for_test(desktop_state_t *desktop,
                                   desktop_pointer_event_t *ev)
{
    mouse_update_pointer(desktop, ev);
}

int mouse_motion_scale_for_test(desktop_state_t *desktop)
{
    return mouse_motion_scale(desktop);
}
#endif

/*
 * Mouse IRQ status bits. PS2_STATUS_AUX_BUFFER is the 8042 controller's
 * "auxiliary output buffer full" flag; when set together with
 * PS2_STATUS_OUT_FULL the byte sitting at PS2_DATA_PORT came from the
 * mouse and we can safely consume it from a mouse-IRQ context.
 */
#define PS2_STATUS_AUX_BUFFER 0x20

static int mouse_irq_should_read_byte(uint8_t status, int bytes_read)
{
    if ((status & PS2_STATUS_OUT_FULL) == 0)
        return 0;
    if (bytes_read < 3)
        return 1;
    return (status & PS2_STATUS_AUX_BUFFER) != 0;
}

static void mouse_handler(void)
{
    desktop_pointer_event_t ev;
    desktop_pointer_event_t pending_ev;
    mouse_packet_t packet;
    desktop_state_t *desktop;
    uint8_t data;
    uint8_t status;
    int have_pending = 0;
    int prev_left_down = -1;
    int bytes_read = 0;

    desktop = desktop_global();

    /*
     * Drain every already-arrived mouse packet inside this IRQ, coalescing
     * pure-motion packets into a single desktop_handle_pointer() call. On
     * fast mouse motion the 8042 can queue several packets between IRQs;
     * the original one-packet-per-IRQ path ran the full render pipeline
     * for each, multiplying drag/flush cost by the packet count.
     *
     * Button-state transitions (click / release) are dispatched
     * immediately so the drag state machine in desktop_handle_pointer()
     * never misses an edge — only the purely-motion intermediate packets
     * are coalesced.
     */
    for (;;) {
        status = port_byte_in(PS2_STATUS_PORT);
        if (!mouse_irq_should_read_byte(status, bytes_read))
            break;

        data = port_byte_in(PS2_DATA_PORT);
        bytes_read++;
        if (mouse_stream_consume(&g_stream, data, &packet) <= 0)
            continue;
        if (mouse_decode_packet(&packet, &ev) != 0)
            continue;

        /* Advance the global pointer so cumulative motion still clamps
         * against the screen bounds exactly like the per-packet path. */
        mouse_update_pointer(desktop, &ev);

        if (prev_left_down != -1 && ev.left_down != prev_left_down) {
            /* Button edge: flush any pending pure-motion event first so
             * the transition isn't reordered past it, then dispatch the
             * edge event unmerged. */
            if (have_pending && desktop_is_active())
                desktop_handle_pointer(desktop, &pending_ev);
            have_pending = 0;
            if (desktop_is_active())
                desktop_handle_pointer(desktop, &ev);
        } else {
            pending_ev = ev;
            have_pending = 1;
        }
        prev_left_down = ev.left_down;
    }

    if (have_pending && desktop_is_active())
        desktop_handle_pointer(desktop, &pending_ev);
}

#ifdef KTEST_ENABLED
int mouse_irq_should_read_byte_for_test(uint8_t status, int bytes_read)
{
    return mouse_irq_should_read_byte(status, bytes_read);
}
#endif

int mouse_init(void)
{
    uint8_t config;

    mouse_stream_reset(&g_stream);
    irq_register(12, mouse_handler);
    irq_unmask(2);
    irq_unmask(12);

    if (ps2_write_command(0xA7) != 0)
        return -1;
    ps2_flush_output();

    int rc = ps2_read_controller_config(&config);
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

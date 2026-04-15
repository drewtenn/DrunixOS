#include "mouse.h"
#include "irq.h"

#define PS2_DATA_PORT 0x60
#define PS2_CMD_PORT  0x64

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);

static uint8_t g_packet_bytes[3];
static int g_packet_index;
static int g_pointer_x = 40;
static int g_pointer_y = 12;

int mouse_decode_packet(const mouse_packet_t *packet, desktop_pointer_event_t *ev)
{
    if (!packet || !ev)
        return -1;

    ev->dx = packet->dx;
    ev->dy = packet->dy;
    ev->left_down = (packet->buttons & 0x1u) != 0;
    return 0;
}

static void mouse_clamp_pointer(desktop_state_t *desktop)
{
    int max_x = 79;
    int max_y = 24;

    if (desktop && desktop->display) {
        max_x = desktop->display->cols - 1;
        max_y = desktop->display->rows - 1;
    }

    if (g_pointer_x < 0)
        g_pointer_x = 0;
    if (g_pointer_y < 0)
        g_pointer_y = 0;
    if (g_pointer_x > max_x)
        g_pointer_x = max_x;
    if (g_pointer_y > max_y)
        g_pointer_y = max_y;
}

static void mouse_handler(void)
{
    mouse_packet_t packet;
    desktop_pointer_event_t ev;
    desktop_state_t *desktop;

    g_packet_bytes[g_packet_index++] = port_byte_in(PS2_DATA_PORT);
    if (g_packet_index < 3)
        return;
    g_packet_index = 0;

    packet.buttons = g_packet_bytes[0];
    packet.dx = (int8_t)g_packet_bytes[1];
    packet.dy = (int8_t)g_packet_bytes[2];
    if (mouse_decode_packet(&packet, &ev) != 0)
        return;

    g_pointer_x += ev.dx;
    g_pointer_y -= ev.dy;

    desktop = desktop_global();
    mouse_clamp_pointer(desktop);
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;

    if (desktop_is_active())
        desktop_handle_pointer(desktop, &ev);
}

int mouse_init(void)
{
    irq_register(12, mouse_handler);
    port_byte_out(PS2_CMD_PORT, 0xA8);
    port_byte_out(PS2_CMD_PORT, 0xD4);
    port_byte_out(PS2_DATA_PORT, 0xF4);
    return 0;
}

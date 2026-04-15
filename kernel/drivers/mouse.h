#ifndef MOUSE_H
#define MOUSE_H

#include "desktop.h"
#include <stdint.h>

typedef struct {
    uint8_t buttons;
    int8_t dx;
    int8_t dy;
} mouse_packet_t;

int mouse_decode_packet(const mouse_packet_t *packet, desktop_pointer_event_t *ev);
int mouse_init(void);

#endif

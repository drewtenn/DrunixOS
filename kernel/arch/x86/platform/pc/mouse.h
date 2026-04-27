#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

typedef struct {
	uint8_t buttons;
	int8_t dx;
	int8_t dy;
} mouse_packet_t;

typedef struct {
	uint8_t bytes[3];
	int index;
} mouse_packet_stream_t;

void mouse_stream_reset(mouse_packet_stream_t *stream);
int mouse_stream_consume(mouse_packet_stream_t *stream,
                         uint8_t data,
                         mouse_packet_t *packet_out);
int mouse_init(void);

#ifdef KTEST_ENABLED
int mouse_irq_should_read_byte_for_test(uint8_t status, int bytes_read);
#endif

#endif

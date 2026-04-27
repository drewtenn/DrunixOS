/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mouse.c — PS/2 mouse IRQ handler.
 *
 * Drains the 8042 controller, reassembles three-byte packets, and
 * forwards each packet to /dev/mouse for a user-space compositor to
 * consume.  The kernel no longer has its own pointer state — that
 * lives in the user-space desktop.
 */

#include "mouse.h"
#include "arch.h"
#include "inputdev.h"
#include "io.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT 0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL 0x02

#define PS2_RESP_ACK 0xFA
#define PS2_RESP_BAT 0xAA
#define PS2_RESP_RESEND 0xFE

#define MOUSE_PACKET_ALWAYS_ONE 0x08u

static mouse_packet_stream_t g_stream;

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
	if (ps2_wait_output_ready() != 0)
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

void mouse_stream_reset(mouse_packet_stream_t *stream)
{
	if (!stream)
		return;
	stream->bytes[0] = 0;
	stream->bytes[1] = 0;
	stream->bytes[2] = 0;
	stream->index = 0;
}

int mouse_stream_consume(mouse_packet_stream_t *stream,
                         uint8_t data,
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
	mouse_packet_t packet;
	uint8_t data;
	uint8_t status;
	int bytes_read = 0;

	for (;;) {
		status = port_byte_in(PS2_STATUS_PORT);
		if (!mouse_irq_should_read_byte(status, bytes_read))
			break;

		data = port_byte_in(PS2_DATA_PORT);
		bytes_read++;
		if (mouse_stream_consume(&g_stream, data, &packet) <= 0)
			continue;
		mousedev_push_packet(packet.buttons,
		                     (uint8_t)packet.dx,
		                     (uint8_t)packet.dy);
	}
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
	arch_irq_register(12, mouse_handler);
	arch_irq_unmask(2);
	arch_irq_unmask(12);

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

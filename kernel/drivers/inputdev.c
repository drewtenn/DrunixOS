/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * inputdev.c — /dev/kbd and /dev/mouse evdev event rings.
 *
 * Both devices carry fixed-size Linux input_event records.  Each
 * report ends in EV_SYN/SYN_REPORT and ring overflow drops whole
 * oldest records, so a slow reader never desynchronises from the
 * producer's framing.
 *
 *   /dev/kbd   — EV_KEY events with `code` set to the raw PS/2
 *                set-1 make code (with KBD_CODE_EXTENDED bit set
 *                for 0xE0-prefixed extended scancodes), value 1 on
 *                press and 0 on release.  Each press or release
 *                is followed by a SYN_REPORT.  No keymap is applied
 *                in the kernel.
 *
 *   /dev/mouse — REL_X / REL_Y / EV_KEY (BTN_LEFT, BTN_RIGHT,
 *                BTN_MIDDLE) events terminated by SYN_REPORT.
 *                REL_Y is published in screen orientation
 *                (positive = down); sign and overflow bits from the
 *                PS/2 wire format are resolved here.
 */

#include "inputdev.h"
#include "arch.h"
#include "chardev.h"
#include "sched.h"
#include <stdint.h>

#define KBD_RING_RECORDS 64u
#define MOUSE_RING_RECORDS 64u

#define MOUSE_PACKET_X_SIGN 0x10u
#define MOUSE_PACKET_Y_SIGN 0x20u
#define MOUSE_PACKET_X_OVERFLOW 0x40u
#define MOUSE_PACKET_Y_OVERFLOW 0x80u

typedef struct {
	input_event_t buf[KBD_RING_RECORDS];
	uint32_t head;
	uint32_t tail;
	uint32_t count;
	wait_queue_t waiters;
} kbd_ring_t;

typedef struct {
	input_event_t buf[MOUSE_RING_RECORDS];
	uint32_t head;
	uint32_t tail;
	uint32_t count;
	wait_queue_t waiters;
	uint8_t prev_buttons;
} mouse_ring_t;

static kbd_ring_t kbd_ring;
static mouse_ring_t mouse_ring;

static int evt_ring_read(input_event_t *ring,
                         uint32_t cap,
                         uint32_t *tail,
                         uint32_t *count,
                         wait_queue_t *waiters,
                         uint8_t *buf,
                         uint32_t bytes)
{
	uint32_t records;
	uint32_t copied = 0;
	uint8_t *out = buf;

	if (!buf)
		return -1;
	if (bytes < sizeof(input_event_t))
		return -1;

	records = bytes / (uint32_t)sizeof(input_event_t);

	while (*count == 0)
		sched_block(waiters);

	while (copied < records && *count > 0) {
		const uint8_t *src = (const uint8_t *)&ring[*tail];

		for (uint32_t i = 0; i < sizeof(input_event_t); i++)
			out[i] = src[i];
		out += sizeof(input_event_t);
		*tail = (*tail + 1u) % cap;
		(*count)--;
		copied++;
	}
	return (int)(copied * sizeof(input_event_t));
}

static void evt_ring_emit(input_event_t *ring,
                          uint32_t cap,
                          uint32_t *head,
                          uint32_t *tail,
                          uint32_t *count,
                          uint16_t type,
                          uint16_t code,
                          int32_t value,
                          uint32_t sec,
                          uint32_t usec)
{
	input_event_t *slot;

	if (*count == cap) {
		*tail = (*tail + 1u) % cap;
		(*count)--;
	}
	slot = &ring[*head];
	slot->sec = sec;
	slot->usec = usec;
	slot->type = type;
	slot->code = code;
	slot->value = value;
	*head = (*head + 1u) % cap;
	(*count)++;
}

static int kbdev_read(uint32_t offset, uint8_t *buf, uint32_t count)
{
	(void)offset;
	return evt_ring_read(kbd_ring.buf,
	                     KBD_RING_RECORDS,
	                     &kbd_ring.tail,
	                     &kbd_ring.count,
	                     &kbd_ring.waiters,
	                     buf,
	                     count);
}

static int mousedev_read(uint32_t offset, uint8_t *buf, uint32_t count)
{
	(void)offset;
	return evt_ring_read(mouse_ring.buf,
	                     MOUSE_RING_RECORDS,
	                     &mouse_ring.tail,
	                     &mouse_ring.count,
	                     &mouse_ring.waiters,
	                     buf,
	                     count);
}

static const chardev_ops_t kbdev_ops = {
    .read_char = 0,
    .write_char = 0,
    .read = kbdev_read,
    .mmap_phys = 0,
};

static const chardev_ops_t mousedev_ops = {
    .read_char = 0,
    .write_char = 0,
    .read = mousedev_read,
    .mmap_phys = 0,
};

void kbdev_push_key(uint16_t code, int32_t value)
{
	uint32_t sec = arch_time_unix_seconds();
	uint32_t usec = arch_time_uptime_ticks();

	evt_ring_emit(kbd_ring.buf,
	              KBD_RING_RECORDS,
	              &kbd_ring.head,
	              &kbd_ring.tail,
	              &kbd_ring.count,
	              EV_KEY,
	              code,
	              value,
	              sec,
	              usec);
	evt_ring_emit(kbd_ring.buf,
	              KBD_RING_RECORDS,
	              &kbd_ring.head,
	              &kbd_ring.tail,
	              &kbd_ring.count,
	              EV_SYN,
	              SYN_REPORT,
	              0,
	              sec,
	              usec);
	sched_wake_all(&kbd_ring.waiters);
}

void mousedev_push_packet(uint8_t flags, uint8_t dx, uint8_t dy)
{
	int32_t rel_x;
	int32_t rel_y;
	uint8_t cur_buttons = flags & 0x07u;
	uint8_t changed = cur_buttons ^ mouse_ring.prev_buttons;
	uint32_t sec = arch_time_unix_seconds();
	uint32_t usec = arch_time_uptime_ticks();

	if (flags & MOUSE_PACKET_X_OVERFLOW)
		rel_x = (flags & MOUSE_PACKET_X_SIGN) ? -127 : 127;
	else
		rel_x = (int32_t)(int8_t)dx;

	if (flags & MOUSE_PACKET_Y_OVERFLOW)
		rel_y = (flags & MOUSE_PACKET_Y_SIGN) ? -127 : 127;
	else
		rel_y = (int32_t)(int8_t)dy;
	rel_y = -rel_y;

	if (rel_x != 0)
		evt_ring_emit(mouse_ring.buf,
		              MOUSE_RING_RECORDS,
		              &mouse_ring.head,
		              &mouse_ring.tail,
		              &mouse_ring.count,
		              EV_REL,
		              REL_X,
		              rel_x,
		              sec,
		              usec);
	if (rel_y != 0)
		evt_ring_emit(mouse_ring.buf,
		              MOUSE_RING_RECORDS,
		              &mouse_ring.head,
		              &mouse_ring.tail,
		              &mouse_ring.count,
		              EV_REL,
		              REL_Y,
		              rel_y,
		              sec,
		              usec);
	if (changed & 0x01u)
		evt_ring_emit(mouse_ring.buf,
		              MOUSE_RING_RECORDS,
		              &mouse_ring.head,
		              &mouse_ring.tail,
		              &mouse_ring.count,
		              EV_KEY,
		              BTN_LEFT,
		              (cur_buttons & 0x01u) ? 1 : 0,
		              sec,
		              usec);
	if (changed & 0x02u)
		evt_ring_emit(mouse_ring.buf,
		              MOUSE_RING_RECORDS,
		              &mouse_ring.head,
		              &mouse_ring.tail,
		              &mouse_ring.count,
		              EV_KEY,
		              BTN_RIGHT,
		              (cur_buttons & 0x02u) ? 1 : 0,
		              sec,
		              usec);
	if (changed & 0x04u)
		evt_ring_emit(mouse_ring.buf,
		              MOUSE_RING_RECORDS,
		              &mouse_ring.head,
		              &mouse_ring.tail,
		              &mouse_ring.count,
		              EV_KEY,
		              BTN_MIDDLE,
		              (cur_buttons & 0x04u) ? 1 : 0,
		              sec,
		              usec);

	evt_ring_emit(mouse_ring.buf,
	              MOUSE_RING_RECORDS,
	              &mouse_ring.head,
	              &mouse_ring.tail,
	              &mouse_ring.count,
	              EV_SYN,
	              SYN_REPORT,
	              0,
	              sec,
	              usec);
	mouse_ring.prev_buttons = cur_buttons;
	sched_wake_all(&mouse_ring.waiters);
}

int kbdev_init(void)
{
	sched_wait_queue_init(&kbd_ring.waiters);
	return chardev_register("kbd", &kbdev_ops);
}

int mousedev_init(void)
{
	sched_wait_queue_init(&mouse_ring.waiters);
	mouse_ring.prev_buttons = 0;
	return chardev_register("mouse", &mousedev_ops);
}

void kbdev_push_event(uint16_t type, uint16_t code, int32_t value)
{
	uint32_t sec = arch_time_unix_seconds();
	uint32_t usec = arch_time_uptime_ticks();

	evt_ring_emit(kbd_ring.buf,
	              KBD_RING_RECORDS,
	              &kbd_ring.head,
	              &kbd_ring.tail,
	              &kbd_ring.count,
	              type,
	              code,
	              value,
	              sec,
	              usec);
	if (type == EV_SYN && code == SYN_REPORT)
		sched_wake_all(&kbd_ring.waiters);
}

void mousedev_push_event(uint16_t type, uint16_t code, int32_t value)
{
	uint32_t sec = arch_time_unix_seconds();
	uint32_t usec = arch_time_uptime_ticks();

	evt_ring_emit(mouse_ring.buf,
	              MOUSE_RING_RECORDS,
	              &mouse_ring.head,
	              &mouse_ring.tail,
	              &mouse_ring.count,
	              type,
	              code,
	              value,
	              sec,
	              usec);
	if (type == EV_SYN && code == SYN_REPORT)
		sched_wake_all(&mouse_ring.waiters);
}

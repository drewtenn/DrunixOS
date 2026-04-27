/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRIVERS_INPUTDEV_H
#define DRIVERS_INPUTDEV_H

#include <stdint.h>

/*
 * /dev/kbd and /dev/mouse: Linux evdev-shaped event devices.
 *
 * Both publish fixed-size input_event records matching the Linux ABI
 * layout (16 bytes on 32-bit targets).  Reports end in
 * EV_SYN/SYN_REPORT and ring overflow drops whole oldest records, so
 * a slow reader can never desynchronise from the producer's framing.
 *
 * The keyboard reports raw PS/2 set-1 make codes as EV_KEY events.
 * Extended (0xE0-prefixed) make codes are encoded with the high bit
 * set in `code` so user space can distinguish, for example,
 * left-arrow (0x80 | 0x4B) from numeric-keypad-4 (0x4B).  value == 1
 * for press and 0 for release.  Translation to ASCII / VT escape
 * sequences and modifier tracking is the user-space compositor's
 * responsibility — the kernel does not synthesise any keymap
 * (matching Linux's evdev model).
 *
 * The mouse reports REL_X, REL_Y, and EV_KEY for BTN_LEFT /
 * BTN_RIGHT / BTN_MIDDLE.  REL_Y is published in screen orientation
 * (positive = down).
 */

typedef struct {
	uint32_t sec;
	uint32_t usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
} input_event_t;

#define EV_SYN 0x00u
#define EV_KEY 0x01u
#define EV_REL 0x02u

#define SYN_REPORT 0x00u

#define REL_X 0x00u
#define REL_Y 0x01u

#define BTN_LEFT 0x110u
#define BTN_RIGHT 0x111u
#define BTN_MIDDLE 0x112u

/*
 * Mark for an extended (0xE0-prefixed) PS/2 make code in a kbd
 * EV_KEY event.  Combine with the make code's low 7 bits to form
 * the EV_KEY `code`.
 */
#define KBD_CODE_EXTENDED 0x80u

int kbdev_init(void);
void kbdev_push_key(uint16_t code, int32_t value);

int mousedev_init(void);
void mousedev_push_packet(uint8_t flags, uint8_t dx, uint8_t dy);

#endif /* DRIVERS_INPUTDEV_H */

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SHARED_KBDMAP_H
#define SHARED_KBDMAP_H

/*
 * Single source of truth for the US-QWERTY scancode-to-byte mapping
 * used by both the in-kernel VT pipeline and the user-space
 * compositor.  The same translation code runs in both contexts so a
 * key that is wired to a control character or VT escape on one side
 * is wired to the same on the other.
 *
 * Inputs match the publish format of /dev/kbd: `code` is a PS/2
 * set-1 make code, with KBDMAP_CODE_EXTENDED set when the original
 * scancode was preceded by 0xE0.  `value` is 1 on press, 0 on
 * release, matching the EV_KEY convention.
 */

#define KBDMAP_CODE_EXTENDED 0x80u

typedef unsigned char kbdmap_u8_t;
typedef unsigned short kbdmap_u16_t;
typedef int kbdmap_i32_t;

typedef struct {
	int shift_held;
	int ctrl_held;
} kbdmap_state_t;

/*
 * Translate one EV_KEY event into a sequence of cooked bytes.
 * Updates *st for shift / ctrl tracking.  Returns the number of
 * bytes written to out (0 if the event produces nothing — releases
 * and modifier presses).  out must hold at least KBDMAP_OUT_MIN
 * bytes; the longest sequence we emit is the five-byte F-key
 * escape.
 */
#define KBDMAP_OUT_MIN 8

int kbdmap_translate(kbdmap_state_t *st,
                     kbdmap_u16_t code,
                     kbdmap_i32_t value,
                     char *out,
                     int outsz);

#endif

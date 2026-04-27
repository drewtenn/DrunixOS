/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * keyboard.c — PS/2 keyboard IRQ.
 *
 * Two pipelines run in parallel, mirroring the Linux kernel:
 *
 *   1. evdev — every press / release is pushed to /dev/kbd as a raw
 *      input_event so a user-space compositor can apply its own
 *      keymap, modifier handling, and shortcuts.
 *
 *   2. VT console — the kernel applies the shared kbdmap, tracks
 *      shift / ctrl, expands function and arrow keys to VT escape
 *      sequences, and feeds the cooked bytes to tty0 so a shell
 *      booted directly on the legacy console (INIT_PROGRAM=bin/shell)
 *      still receives input without the compositor.
 *
 * The translation tables and shift / ctrl state live in
 * shared/kbdmap.c so the kernel and the user-space compositor stay
 * keystroke-for-keystroke identical.
 */

#include <stdint.h>
#include "arch.h"
#include "io.h"
#include "inputdev.h"
#include "kbdmap.h"
#include "tty.h"
#include "keyboard.h"

#define KEYBOARD_DATA_PORT 0x60

/*
 * 0xE0 introduces a two-byte extended scancode (e.g. arrow keys,
 * keypad enter, the right-hand modifiers).  Carried across IRQs
 * so the next byte resolves correctly.
 */
static int e0_prefix;
static kbdmap_state_t vt_kbd_state;

void keyboard_handler(void)
{
	uint8_t scancode = port_byte_in(KEYBOARD_DATA_PORT);
	uint8_t make;
	int press;
	uint16_t keymap_code;
	char cooked[KBDMAP_OUT_MIN];
	int produced;

	if (scancode == 0xE0u) {
		e0_prefix = 1;
		return;
	}

	make = scancode & 0x7Fu;
	press = (scancode & 0x80u) == 0;

	if (make == 0)
		goto reset_prefix;

	keymap_code = make;
	if (e0_prefix)
		keymap_code |= KBDMAP_CODE_EXTENDED;

	/* evdev: raw press / release for the user-space compositor. */
	kbdev_push_key(keymap_code, press ? 1 : 0);

	/* VT console: cooked bytes for tty0 via the shared keymap. */
	produced = kbdmap_translate(
	    &vt_kbd_state, keymap_code, press ? 1 : 0, cooked, (int)sizeof(cooked));
	for (int i = 0; i < produced; i++)
		tty_input_char(0, cooked[i]);

reset_prefix:
	e0_prefix = 0;
}

void keyboard_init(void)
{
	arch_irq_register(1, keyboard_handler);
}

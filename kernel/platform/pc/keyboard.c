/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * keyboard.c — PS/2 keyboard IRQ handling and scancode-to-input translation.
 */

#include <stdint.h>
#include "arch.h"
#include "chardev.h"
#include "sched.h"
#include "desktop.h"
#include "tty.h"
#include "io.h"
#include "keyboard.h"

#define KEYBOARD_DATA_PORT 0x60

/* US QWERTY scancode set 1 → ASCII (unshifted). Index = make code (0x01–0x39). */
static const char scancode_ascii[] = {
    0,  /* 0x00 */
    27, /* 0x01 ESC */
    '1',  '2', '3', '4',  '5', '6',  '7',
    '8',  '9', '0', '-',  '=', '\b', /* 0x0E backspace */
    '\t',                            /* 0x0F tab */
    'q',  'w', 'e', 'r',  't', 'y',  'u',
    'i',  'o', 'p', '[',  ']', '\r', /* 0x1C enter */
    0,                               /* 0x1D left ctrl */
    'a',  's', 'd', 'f',  'g', 'h',  'j',
    'k',  'l', ';', '\'', '`', 0, /* 0x2A left shift */
    '\\', 'z', 'x', 'c',  'v', 'b',  'n',
    'm',  ',', '.', '/',  0, /* 0x36 right shift */
    '*',  0,                 /* 0x38 left alt */
    ' '                      /* 0x39 space */
};

/* Shifted equivalents for the same scancode positions. */
static const char scancode_ascii_shifted[] = {
    0,  /* 0x00 */
    27, /* 0x01 ESC */
    '!',  '@', '#', '$', '%', '^',  '&',
    '*',  '(', ')', '_', '+', '\b', /* 0x0E backspace */
    '\t',                           /* 0x0F tab */
    'Q',  'W', 'E', 'R', 'T', 'Y',  'U',
    'I',  'O', 'P', '{', '}', '\r', /* 0x1C enter */
    0,                              /* 0x1D left ctrl */
    'A',  'S', 'D', 'F', 'G', 'H',  'J',
    'K',  'L', ':', '"', '~', 0, /* 0x2A left shift */
    '|',  'Z', 'X', 'C', 'V', 'B',  'N',
    'M',  '<', '>', '?', 0, /* 0x36 right shift */
    '*',  0,                /* 0x38 left alt */
    ' '                     /* 0x39 space */
};

#define SCANCODE_MAX (sizeof(scancode_ascii) / sizeof(scancode_ascii[0]))

/* Ring buffer */
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static int kb_head = 0;
static int kb_tail = 0;

static int e0_prefix = 0;  /* set when 0xE0 extended scancode prefix is seen */
static int shift_held = 0; /* non-zero while either shift key is depressed */
static int ctrl_held = 0;  /* non-zero while Ctrl is depressed */

typedef struct {
	uint8_t scancode;
	const char *sequence;
} keyboard_sequence_t;

static const keyboard_sequence_t normal_sequences[] = {
    {0x3b, "\x1bOP"},   /* F1 */
    {0x3c, "\x1bOQ"},   /* F2 */
    {0x3d, "\x1bOR"},   /* F3 */
    {0x3e, "\x1bOS"},   /* F4 */
    {0x3f, "\x1b[15~"}, /* F5 */
    {0x40, "\x1b[17~"}, /* F6 */
    {0x41, "\x1b[18~"}, /* F7 */
    {0x42, "\x1b[19~"}, /* F8 */
    {0x43, "\x1b[20~"}, /* F9 */
    {0x44, "\x1b[21~"}, /* F10 */
    {0x57, "\x1b[23~"}, /* F11 */
    {0x58, "\x1b[24~"}, /* F12 */
};

static const keyboard_sequence_t extended_sequences[] = {
    {0x47, "\x1b[H"},  /* Home */
    {0x48, "\x1b[A"},  /* Up */
    {0x49, "\x1b[5~"}, /* Page Up */
    {0x4b, "\x1b[D"},  /* Left */
    {0x4d, "\x1b[C"},  /* Right */
    {0x4f, "\x1b[F"},  /* End */
    {0x50, "\x1b[B"},  /* Down */
    {0x51, "\x1b[6~"}, /* Page Down */
    {0x52, "\x1b[2~"}, /* Insert */
    {0x53, "\x1b[3~"}, /* Delete */
};

int keyboard_ascii_for_scancode_for_test(uint8_t scancode,
                                         int shift_down,
                                         int ctrl_down,
                                         char *out)
{
	char c;

	if (!out || scancode >= SCANCODE_MAX)
		return 0;
	c = shift_down ? scancode_ascii_shifted[scancode]
	               : scancode_ascii[scancode];
	if (!c)
		return 0;
	if (ctrl_down) {
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 1);
		else if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 1);
	}
	*out = c;
	return 1;
}

static const char *keyboard_sequence_for_scancode(uint8_t scancode,
                                                  int extended)
{
	const keyboard_sequence_t *sequences;
	uint32_t count;
	uint32_t i;

	sequences = extended ? extended_sequences : normal_sequences;
	count = extended ? (uint32_t)(sizeof(extended_sequences) /
	                              sizeof(extended_sequences[0]))
	                 : (uint32_t)(sizeof(normal_sequences) /
	                              sizeof(normal_sequences[0]));

	for (i = 0; i < count; i++) {
		if (sequences[i].scancode == scancode)
			return sequences[i].sequence;
	}
	return 0;
}

int keyboard_sequence_for_scancode_for_test(uint8_t scancode,
                                            int extended,
                                            const char **out)
{
	const char *sequence;

	if (!out)
		return 0;
	sequence = keyboard_sequence_for_scancode(scancode, extended);
	if (!sequence)
		return 0;
	*out = sequence;
	return 1;
}

static void keyboard_deliver_char(char c)
{
	if (desktop_is_active() &&
	    desktop_handle_key(desktop_global(), c) == DESKTOP_KEY_CONSUMED)
		return;
	tty_input_char(0, c);
}

static void keyboard_deliver_sequence(const char *sequence)
{
	desktop_state_t *desktop;

	if (!sequence)
		return;

	desktop = desktop_is_active() ? desktop_global() : 0;
	if (desktop &&
	    desktop_handle_key_sequence(desktop, sequence) == DESKTOP_KEY_CONSUMED)
		return;

	while (*sequence) {
		tty_input_char(0, *sequence);
		sequence++;
	}
}

static void kb_push(char c)
{
	int next = (kb_head + 1) % KB_BUFFER_SIZE;
	if (next != kb_tail) {
		kb_buffer[kb_head] = c;
		kb_head = next;
	}
}

char kb_getchar(void)
{
	if (kb_head == kb_tail)
		return 0;
	char c = kb_buffer[kb_tail];
	kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
	return c;
}

/* Called from the IRQ dispatch table when IRQ1 fires */
void keyboard_handler(void)
{
	unsigned char scancode = port_byte_in(KEYBOARD_DATA_PORT);

	/* 0xE0 introduces a two-byte extended scancode (e.g. Page Up, Page Down). */
	if (scancode == 0xE0) {
		e0_prefix = 1;
		return;
	}

	if (scancode & 0x80) { /* key release */
		unsigned char make = scancode & 0x7F;
		if (make == 0x2A || make == 0x36) /* left/right shift released */
			shift_held = 0;
		if (make == 0x1D) /* left Ctrl released */
			ctrl_held = 0;
		e0_prefix = 0;
		return;
	}

	if (e0_prefix) {
		const char *sequence;

		e0_prefix = 0;
		if (scancode == 0x1D) {
			ctrl_held = 1;
			return;
		}

		sequence = keyboard_sequence_for_scancode(scancode, 1);
		if (sequence)
			keyboard_deliver_sequence(sequence);
		return;
	}

	/* Modifier key press — track state, don't emit a character. */
	if (scancode == 0x2A || scancode == 0x36) {
		shift_held = 1;
		return;
	}
	if (scancode == 0x1D) {
		ctrl_held = 1;
		return;
	}

	{
		const char *sequence = keyboard_sequence_for_scancode(scancode, 0);
		if (sequence) {
			keyboard_deliver_sequence(sequence);
			return;
		}
	}

	if (scancode == 0x45 || scancode == 0x46) {
		/* Num Lock and Scroll Lock have no terminal byte representation yet. */
		return;
	}

	if (scancode < SCANCODE_MAX) {
		char c;
		if (keyboard_ascii_for_scancode_for_test(
		        scancode, shift_held, ctrl_held, &c))
			keyboard_deliver_char(c);
	}
}

static const chardev_ops_t kb_chardev_ops = {
    .read_char = kb_getchar,
    .write_char = 0,
};

/* Register the keyboard IRQ handler and chardev ops with the kernel. */
void keyboard_init(void)
{
	arch_irq_register(1, keyboard_handler);
	chardev_register("stdin", &kb_chardev_ops);
	chardev_register("tty0", &kb_chardev_ops);
}

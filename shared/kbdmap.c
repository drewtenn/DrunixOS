/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kbdmap.c — US-QWERTY scancode → ASCII / VT escape translation,
 * shared between the kernel's VT keyboard handler and the user-space
 * compositor.
 */

#include "kbdmap.h"

#define MOD_LSHIFT 0x2A
#define MOD_RSHIFT 0x36
#define MOD_LCTRL 0x1D

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

typedef struct {
	kbdmap_u8_t scancode;
	const char *sequence;
} kbdmap_seq_t;

static const kbdmap_seq_t normal_sequences[] = {
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

static const kbdmap_seq_t extended_sequences[] = {
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

static int seq_lookup(const kbdmap_seq_t *table,
                      int count,
                      kbdmap_u8_t scancode,
                      const char **out)
{
	for (int i = 0; i < count; i++) {
		if (table[i].scancode == scancode) {
			*out = table[i].sequence;
			return 1;
		}
	}
	return 0;
}

static int copy_seq(const char *seq, char *out, int outsz)
{
	int n = 0;

	while (seq[n] && n < outsz)
		n++;
	for (int i = 0; i < n; i++)
		out[i] = seq[i];
	return n;
}

int kbdmap_translate(kbdmap_state_t *st,
                     kbdmap_u16_t code,
                     kbdmap_i32_t value,
                     char *out,
                     int outsz)
{
	kbdmap_u8_t make = (kbdmap_u8_t)(code & 0x7Fu);
	int extended = (code & KBDMAP_CODE_EXTENDED) != 0;
	const char *seq;
	char c;

	if (!st || !out || outsz <= 0)
		return 0;

	if (!extended && (make == MOD_LSHIFT || make == MOD_RSHIFT)) {
		st->shift_held = (value != 0);
		return 0;
	}
	if (make == MOD_LCTRL) {
		st->ctrl_held = (value != 0);
		return 0;
	}

	if (value == 0)
		return 0;

	if (extended && seq_lookup(extended_sequences,
	                           (int)(sizeof(extended_sequences) /
	                                 sizeof(extended_sequences[0])),
	                           make,
	                           &seq))
		return copy_seq(seq, out, outsz);

	if (!extended && seq_lookup(normal_sequences,
	                            (int)(sizeof(normal_sequences) /
	                                  sizeof(normal_sequences[0])),
	                            make,
	                            &seq))
		return copy_seq(seq, out, outsz);

	if (extended || make >= SCANCODE_MAX)
		return 0;

	c = st->shift_held ? scancode_ascii_shifted[make] : scancode_ascii[make];
	if (!c)
		return 0;

	if (st->ctrl_held) {
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 1);
		else if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 1);
	}

	out[0] = c;
	return 1;
}

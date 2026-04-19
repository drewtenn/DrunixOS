/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * keyboard.c — PS/2 keyboard IRQ handling and scancode-to-input translation.
 */

#include <stdint.h>
#include "irq.h"
#include "chardev.h"
#include "sched.h"
#include "desktop.h"
#include "tty.h"

#define KEYBOARD_DATA_PORT 0x60

extern void          port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);

/* US QWERTY scancode set 1 → ASCII (unshifted). Index = make code (0x01–0x39). */
static const char scancode_ascii[] = {
    0,    /* 0x00 */
    27,   /* 0x01 ESC */
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', /* 0x0E backspace */
    '\t', /* 0x0F tab */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\r', /* 0x1C enter */
    0,    /* 0x1D left ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    /* 0x2A left shift */
    '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,    /* 0x36 right shift */
    '*',
    0,    /* 0x38 left alt */
    ' '   /* 0x39 space */
};

/* Shifted equivalents for the same scancode positions. */
static const char scancode_ascii_shifted[] = {
    0,    /* 0x00 */
    27,   /* 0x01 ESC */
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', /* 0x0E backspace */
    '\t', /* 0x0F tab */
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\r', /* 0x1C enter */
    0,    /* 0x1D left ctrl */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    /* 0x2A left shift */
    '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,    /* 0x36 right shift */
    '*',
    0,    /* 0x38 left alt */
    ' '   /* 0x39 space */
};

#define SCANCODE_MAX (sizeof(scancode_ascii) / sizeof(scancode_ascii[0]))

/* Ring buffer */
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static int  kb_head = 0;
static int  kb_tail = 0;

static int e0_prefix  = 0;  /* set when 0xE0 extended scancode prefix is seen */
static int shift_held = 0;  /* non-zero while either shift key is depressed */
static int ctrl_held  = 0;  /* non-zero while Ctrl is depressed */

int keyboard_ascii_for_scancode_for_test(uint8_t scancode, int shift_down,
                                         int ctrl_down, char *out)
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

static void keyboard_deliver_char(char c)
{
    if (desktop_is_active() &&
        desktop_handle_key(desktop_global(), c) == DESKTOP_KEY_CONSUMED)
        return;
    tty_input_char(0, c);
}

static void kb_push(char c) {
    int next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next != kb_tail) {
        kb_buffer[kb_head] = c;
        kb_head = next;
    }
}

char kb_getchar() {
    if (kb_head == kb_tail) return 0;
    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return c;
}

/* Called from the IRQ dispatch table when IRQ1 fires */
void keyboard_handler(void) {
    unsigned char scancode = port_byte_in(KEYBOARD_DATA_PORT);

    /* 0xE0 introduces a two-byte extended scancode (e.g. Page Up, Page Down). */
    if (scancode == 0xE0) {
        e0_prefix = 1;
        return;
    }

    if (scancode & 0x80) {   /* key release */
        unsigned char make = scancode & 0x7F;
        if (make == 0x2A || make == 0x36)   /* left/right shift released */
            shift_held = 0;
        if (make == 0x1D)                   /* left Ctrl released */
            ctrl_held = 0;
        e0_prefix = 0;
        return;
    }

    /* Modifier key press — track state, don't emit a character. */
    if (scancode == 0x2A || scancode == 0x36) { shift_held = 1; return; }
    if (scancode == 0x1D) { ctrl_held = 1; return; }

    if (e0_prefix) {
        e0_prefix = 0;
        if (scancode == 0x49) keyboard_deliver_char('\x01');  /* Page Up   → SOH */
        if (scancode == 0x51) keyboard_deliver_char('\x02');  /* Page Down → STX */
        if (scancode == 0x53) keyboard_deliver_char(0x7F);    /* Delete    → DEL */
        return;
    }

    if (scancode < SCANCODE_MAX) {
        char c;
        if (keyboard_ascii_for_scancode_for_test(scancode, shift_held,
                                                 ctrl_held, &c))
            keyboard_deliver_char(c);
    }
}

static const chardev_ops_t kb_chardev_ops = {
    .read_char  = kb_getchar,
    .write_char = 0,
};

/* Register the keyboard IRQ handler and chardev ops with the kernel. */
void keyboard_init(void) {
    irq_register(1, keyboard_handler);
    chardev_register("stdin", &kb_chardev_ops);
    chardev_register("tty0",  &kb_chardev_ops);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

int keyboard_ascii_for_scancode_for_test(uint8_t scancode,
                                         int shift_down,
                                         int ctrl_down,
                                         char *out);
int keyboard_sequence_for_scancode_for_test(uint8_t scancode,
                                            int extended,
                                            const char **out);
char kb_getchar(void);
void keyboard_handler(void);
void keyboard_init(void);

#endif

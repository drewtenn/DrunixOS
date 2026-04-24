/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_CONSOLE_RUNTIME_H
#define KERNEL_CONSOLE_RUNTIME_H

#include <stdint.h>

struct process;
typedef struct process process_t;

int console_runtime_clear(void);
int console_runtime_scroll(int rows);
int console_runtime_write_feedback(const char *buf, uint32_t len);
uintptr_t console_runtime_begin_process_output(const process_t *proc);
void console_runtime_end_process_output(uintptr_t batch_token);
int console_runtime_write_process_output(const process_t *proc,
                                         const char *buf,
                                         uint32_t len);
void console_runtime_winsize(uint16_t *rows_out, uint16_t *cols_out);

#endif

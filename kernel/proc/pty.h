/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PROC_PTY_H
#define PROC_PTY_H

#include <stdint.h>

/*
 * Pseudo-terminal pairs.
 *
 * A pty is two independent byte streams:
 *   master->slave: bytes the compositor sends; the slave reads them as
 *                  if they came from a keyboard.
 *   slave->master: bytes the program (shell) writes; the compositor
 *                  reads them as if it were the screen.
 *
 * The slave end is mounted at /dev/pts<N> and can be dup'd as a
 * controlling terminal for a child process; the master end is opened
 * as /dev/ptmx, which atomically allocates a free pair and binds the
 * caller as its master.
 *
 * Line discipline is intentionally raw: the compositor handles its
 * own echoing, line editing, and signal generation.
 */

#define PTY_MAX 4

int pty_alloc_master(void);
int pty_open_slave(uint32_t pty_idx);
void pty_get_master(uint32_t pty_idx);
void pty_get_slave(uint32_t pty_idx);
void pty_release_master(uint32_t pty_idx);
void pty_release_slave(uint32_t pty_idx);

int pty_master_read(uint32_t pty_idx, uint8_t *buf, uint32_t count);
int pty_master_write(uint32_t pty_idx, const uint8_t *buf, uint32_t count);
int pty_slave_read(uint32_t pty_idx, uint8_t *buf, uint32_t count);
int pty_slave_write(uint32_t pty_idx, const uint8_t *buf, uint32_t count);
uint32_t pty_master_read_available(uint32_t pty_idx);
uint32_t pty_slave_read_available(uint32_t pty_idx);
int pty_master_read_closed(uint32_t pty_idx);
int pty_slave_read_closed(uint32_t pty_idx);

#endif /* PROC_PTY_H */

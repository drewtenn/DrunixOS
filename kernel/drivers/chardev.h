/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef CHARDEV_H
#define CHARDEV_H

#include <stdint.h>

/*
 * Character device registry.
 *
 * Drivers call chardev_register() once during init to publish their
 * read/write ops-table under a short name (e.g. "stdin", "tty0").
 * The syscall layer calls chardev_get() rather than calling driver
 * functions directly.
 */

#define CHARDEV_NAME_MAX 8 /* max name length including NUL */
#define CHARDEV_MAX 4      /* max registered char devices */

typedef struct {
	/* Read one character; returns 0 if no data is available (non-blocking). */
	char (*read_char)(void);

	/* Write one character.  May be NULL for input-only devices. */
	void (*write_char)(char c);
} chardev_ops_t;

/*
 * Register a device.  name is copied internally.
 * Returns 0 on success, -1 if the registry is full.
 */
int chardev_register(const char *name, const chardev_ops_t *ops);

/*
 * Look up a device by name.  Returns a pointer to the ops-table, or NULL
 * if no device with that name has been registered.
 */
const chardev_ops_t *chardev_get(const char *name);

#endif

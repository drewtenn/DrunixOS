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
#define CHARDEV_MAX 8      /* max registered char devices */

typedef struct {
	/*
	 * Read one character; returns 0 if no data is available (non-blocking).
	 * May be NULL for devices that are not byte-readable (e.g. /dev/fb0).
	 */
	char (*read_char)(void);

	/* Write one character.  May be NULL for input-only devices. */
	void (*write_char)(char c);

	/*
	 * Blocking multi-byte read.  Returns the number of bytes copied into
	 * buf (1..count) on success, 0 if the device is at end-of-stream, or
	 * a negative value on error.  Drivers that implement this op should
	 * sleep on their own wait queue when no data is available.
	 *
	 * `offset` is the per-fd cursor.  Stream devices ignore it; positioned
	 * devices (e.g. fb0info) honour it and return 0 once exhausted.  The
	 * fd dispatch advances the cursor by the returned byte count.
	 *
	 * If both `read` and `read_char` are NULL the device is not readable
	 * (e.g. /dev/fb0).  When both are present, callers prefer `read`.
	 */
	int (*read)(uint32_t offset, uint8_t *buf, uint32_t count);

	/*
	 * Map a contiguous range of the device into a process address space.
	 *
	 * The driver translates a byte offset and length into a physical base
	 * address.  The mmap syscall path then installs page-table entries for
	 * `length` bytes starting at the returned physical address.
	 *
	 * `prot` carries the requested LINUX_PROT_* bits so a driver may reject
	 * unsupported access modes (for example, write to a read-only aperture).
	 *
	 * Returns 0 on success and writes the contiguous physical base to
	 * *phys_out.  Returns -1 on bounds, alignment, or permission errors.
	 *
	 * May be NULL for devices that do not support mmap.
	 */
	int (*mmap_phys)(uint32_t offset,
	                 uint32_t length,
	                 uint32_t prot,
	                 uint64_t *phys_out);
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

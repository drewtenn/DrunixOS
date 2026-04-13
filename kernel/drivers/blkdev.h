/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

/*
 * Block device registry.
 *
 * Drivers call blkdev_register() once during init to publish their
 * read/write ops-table under a short name (e.g. "hd0").  Consumers
 * (filesystems, ELF loader, syscall layer) call blkdev_get() to
 * retrieve the ops-table rather than calling driver functions directly.
 */

#define BLKDEV_NAME_MAX  8    /* max name length including NUL */
#define BLKDEV_MAX       4    /* max registered block devices */
#define BLKDEV_SECTOR_SIZE 512

typedef struct {
    /* Read one BLKDEV_SECTOR_SIZE-byte sector from the device.
     * Returns 0 on success, non-zero on error. */
    int (*read_sector)(uint32_t lba, uint8_t *buf);

    /* Write one BLKDEV_SECTOR_SIZE-byte sector to the device.
     * Returns 0 on success, non-zero on error. */
    int (*write_sector)(uint32_t lba, const uint8_t *buf);
} blkdev_ops_t;

/*
 * Register a device.  name is copied internally (up to BLKDEV_NAME_MAX-1
 * bytes).  Returns 0 on success, -1 if the registry is full.
 */
int blkdev_register(const char *name, const blkdev_ops_t *ops);

/*
 * Look up a device by name.  Returns a pointer to the ops-table, or NULL
 * if no device with that name has been registered.
 */
const blkdev_ops_t *blkdev_get(const char *name);

#endif

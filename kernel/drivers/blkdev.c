/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * blkdev.c — block device registry and dispatch helpers.
 */

#include "blkdev.h"

static struct {
    char             name[BLKDEV_NAME_MAX];
    const blkdev_ops_t *ops;
} blkdev_table[BLKDEV_MAX];

int blkdev_register(const char *name, const blkdev_ops_t *ops) {
    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_table[i].name[0] == '\0') {
            int j = 0;
            while (j < BLKDEV_NAME_MAX - 1 && name[j]) {
                blkdev_table[i].name[j] = name[j];
                j++;
            }
            blkdev_table[i].name[j] = '\0';
            blkdev_table[i].ops = ops;
            return 0;
        }
    }
    return -1;
}

const blkdev_ops_t *blkdev_get(const char *name) {
    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_table[i].name[0] == '\0') continue;
        int j = 0;
        while (j < BLKDEV_NAME_MAX && name[j] && blkdev_table[i].name[j] == name[j])
            j++;
        if (blkdev_table[i].name[j] == '\0' && name[j] == '\0')
            return blkdev_table[i].ops;
    }
    return 0;
}

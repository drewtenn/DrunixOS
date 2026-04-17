/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"
#include <stdint.h>

typedef enum {
    PROCFS_FILE_NONE    = 0,
    PROCFS_FILE_STATUS  = 1,
    PROCFS_FILE_MAPS    = 2,
    PROCFS_FILE_FD      = 3,
    PROCFS_FILE_MODULES = 4,
    PROCFS_FILE_KMSG    = 5,
    PROCFS_FILE_VMSTAT  = 6,
    PROCFS_FILE_FAULT   = 7,
    PROCFS_FILE_MOUNTS  = 8,
} procfs_file_kind_t;

int procfs_fill_node(const char *relpath, vfs_node_t *node_out);
int procfs_stat(const char *relpath, vfs_stat_t *st);
int procfs_getdents(const char *relpath, char *buf, uint32_t bufsz);
int procfs_file_size(uint32_t kind, uint32_t pid, uint32_t index,
                     uint32_t *size_out);
int procfs_read_file(uint32_t kind, uint32_t pid, uint32_t index,
                     uint32_t offset, char *buf, uint32_t count);

#endif

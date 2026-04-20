/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SYSFS_H
#define SYSFS_H

#include "vfs.h"
#include <stdint.h>

int sysfs_fill_node(const char *relpath, vfs_node_t *node_out);
int sysfs_stat(const char *relpath, vfs_stat_t *st);
int sysfs_getdents(const char *relpath, char *buf, uint32_t bufsz);
int sysfs_file_size(uint32_t inode_num, uint32_t *size_out);
int sysfs_read_file(uint32_t inode_num,
                    uint32_t offset,
                    char *buf,
                    uint32_t count);

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

#define BLKDEV_NAME_MAX  12
#define BLKDEV_MAX       16
#define BLKDEV_SECTOR_SIZE 512
#define BLKDEV_NO_PARENT 0xFFFFFFFFu

typedef enum {
    BLKDEV_KIND_DISK = 1,
    BLKDEV_KIND_PART = 2,
} blkdev_kind_t;

typedef struct {
    int (*read_sector)(uint32_t lba, uint8_t *buf);
    int (*write_sector)(uint32_t lba, const uint8_t *buf);
} blkdev_ops_t;

typedef struct {
    char     name[BLKDEV_NAME_MAX];
    uint32_t kind;
    uint32_t sector_size;
    uint32_t sectors;
    uint32_t readonly;
    uint32_t major;
    uint32_t minor;
    uint32_t parent_index;
    uint32_t start_sector;
    uint32_t partition_number;
} blkdev_info_t;

void blkdev_reset(void);
int blkdev_register_disk(const char *name, uint32_t major, uint32_t minor,
                         uint32_t sectors, const blkdev_ops_t *ops);
int blkdev_register_part(const char *name, uint32_t parent_index,
                         uint32_t partition_number, uint32_t start_sector,
                         uint32_t sectors);
int blkdev_register(const char *name, const blkdev_ops_t *ops);
int blkdev_scan_mbr(uint32_t disk_index);
const blkdev_ops_t *blkdev_get(const char *name);
const blkdev_ops_t *blkdev_ops_at(uint32_t index);
uint32_t blkdev_count(void);
int blkdev_info_at(uint32_t index, blkdev_info_t *out);
int blkdev_info_for_index(uint32_t index, blkdev_info_t *out);
int blkdev_find_index(const char *name);

#endif

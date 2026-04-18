/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * blkdev.c — block device registry and dispatch helpers.
 */

#include "blkdev.h"
#include "kstring.h"

const blkdev_ops_t *blkdev_part_ops_for_index(uint32_t index);

typedef struct {
    blkdev_info_t info;
    const blkdev_ops_t *ops;
} blkdev_entry_t;

static blkdev_entry_t blkdev_table[BLKDEV_MAX];

static int blkdev_name_equals(const char *a, const char *b)
{
    return k_strcmp(a, b) == 0;
}

static int blkdev_name_is_valid(const char *name)
{
    uint32_t len;

    if (!name)
        return 0;
    len = k_strnlen(name, BLKDEV_NAME_MAX);
    return len != 0 && len < BLKDEV_NAME_MAX;
}

void blkdev_reset(void)
{
    k_memset(blkdev_table, 0, sizeof(blkdev_table));
}

static int blkdev_alloc_slot(void)
{
    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_table[i].info.name[0] == '\0')
            return i;
    }
    return -1;
}

int blkdev_register_disk(const char *name, uint32_t major, uint32_t minor,
                         uint32_t sectors, const blkdev_ops_t *ops)
{
    int idx;

    if (!blkdev_name_is_valid(name) || !ops || sectors == 0 || blkdev_find_index(name) >= 0)
        return -1;
    idx = blkdev_alloc_slot();
    if (idx < 0)
        return -1;

    k_strncpy(blkdev_table[idx].info.name, name, BLKDEV_NAME_MAX);
    blkdev_table[idx].info.kind = BLKDEV_KIND_DISK;
    blkdev_table[idx].info.sector_size = BLKDEV_SECTOR_SIZE;
    blkdev_table[idx].info.sectors = sectors;
    blkdev_table[idx].info.readonly = 0;
    blkdev_table[idx].info.major = major;
    blkdev_table[idx].info.minor = minor;
    blkdev_table[idx].info.parent_index = BLKDEV_NO_PARENT;
    blkdev_table[idx].info.start_sector = 0;
    blkdev_table[idx].info.partition_number = 0;
    blkdev_table[idx].ops = ops;
    return 0;
}

int blkdev_register_part(const char *name, uint32_t parent_index,
                         uint32_t partition_number, uint32_t start_sector,
                         uint32_t sectors)
{
    int idx;
    const blkdev_entry_t *parent;

    if (!blkdev_name_is_valid(name) || sectors == 0 || parent_index >= BLKDEV_MAX ||
        blkdev_find_index(name) >= 0)
        return -1;
    parent = &blkdev_table[parent_index];
    if (parent->info.name[0] == '\0' || parent->info.kind != BLKDEV_KIND_DISK)
        return -1;
    if (start_sector >= parent->info.sectors ||
        sectors > parent->info.sectors - start_sector)
        return -1;

    idx = blkdev_alloc_slot();
    if (idx < 0)
        return -1;
    k_strncpy(blkdev_table[idx].info.name, name, BLKDEV_NAME_MAX);
    blkdev_table[idx].info.kind = BLKDEV_KIND_PART;
    blkdev_table[idx].info.sector_size = BLKDEV_SECTOR_SIZE;
    blkdev_table[idx].info.sectors = sectors;
    blkdev_table[idx].info.readonly = parent->info.readonly;
    blkdev_table[idx].info.major = parent->info.major;
    blkdev_table[idx].info.minor = parent->info.minor + partition_number;
    blkdev_table[idx].info.parent_index = parent_index;
    blkdev_table[idx].info.start_sector = start_sector;
    blkdev_table[idx].info.partition_number = partition_number;
    blkdev_table[idx].ops = blkdev_part_ops_for_index((uint32_t)idx);
    if (!blkdev_table[idx].ops)
        return -1;
    return 0;
}

int blkdev_register(const char *name, const blkdev_ops_t *ops)
{
    return blkdev_register_disk(name, 0, 0, 0xFFFFFFFFu, ops) >= 0 ? 0 : -1;
}

const blkdev_ops_t *blkdev_get(const char *name)
{
    int idx = blkdev_find_index(name);
    return idx >= 0 ? blkdev_table[idx].ops : 0;
}

const blkdev_ops_t *blkdev_ops_at(uint32_t index)
{
    if (index >= BLKDEV_MAX || blkdev_table[index].info.name[0] == '\0')
        return 0;
    return blkdev_table[index].ops;
}

uint32_t blkdev_count(void)
{
    uint32_t count = 0;
    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_table[i].info.name[0] != '\0')
            count++;
    }
    return count;
}

int blkdev_info_at(uint32_t index, blkdev_info_t *out)
{
    uint32_t seen = 0;

    if (!out)
        return -1;
    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_table[i].info.name[0] == '\0')
            continue;
        if (seen == index) {
            *out = blkdev_table[i].info;
            return 0;
        }
        seen++;
    }
    return -1;
}

int blkdev_find_index(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_table[i].info.name[0] != '\0' &&
            blkdev_name_equals(blkdev_table[i].info.name, name))
            return i;
    }
    return -1;
}

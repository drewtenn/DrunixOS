# Linux-Style Lsblk Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build Linux-style block-device discovery in Drunix, including MBR-backed `sda`/`sdb` disks, partition devices, `/dev/sd*`, `/sys/block`, accurate `/proc/mounts`, and a userland `lsblk` with default and `-f` output.

**Architecture:** Extend the existing block registry into an enumerable storage topology, with ATA registering whole disks and an MBR scanner registering partition children. Mount filesystems from partition devices, expose the topology through a narrow sysfs backend, and keep `lsblk` as a userland formatter over `/sys/block`, `/proc/mounts`, and filesystem probes.

**Tech Stack:** Freestanding C kernel, existing VFS/procfs/devfs patterns, ATA PIO block driver, Python image builders, freestanding C/C++ userland, in-kernel KTEST, Makefile/QEMU verification.

---

## Scope Check

This spec touches block devices, disk images, VFS synthetic filesystems, boot mounting, and userland. These are coupled by one user-visible behavior: Linux-style block listing. The plan keeps them in one implementation sequence because no single subsystem produces a useful working result alone. Each task still ends in a testable state and a commit.

## File Structure

- Create `kernel/drivers/blkdev_part.c`: MBR parsing, partition child registration, and partition read/write wrappers.
- Modify `kernel/drivers/blkdev.h`: public block metadata types, registry enumeration API, explicit disk/partition registration API.
- Modify `kernel/drivers/blkdev.c`: enumerable block table, metadata storage, whole-disk registration, partition registration, lookup by public name.
- Modify `kernel/drivers/ata.c`: register `sda` and `sdb` as disks with sector counts.
- Modify `kernel/drivers/ata.h`: expose disk registration behavior with Linux-style names.
- Create `kernel/test/test_blkdev.c`: unit tests for block enumeration, MBR parsing, and partition offset bounds.
- Modify `kernel/test/ktest.h`, `kernel/test/ktest.c`, `Makefile`: include the new KTEST suite and new block/sysfs objects.
- Leave `tools/mkext3.py` and `tools/mkfs.py` as filesystem payload builders; wrap their outputs with `tools/wrap_mbr.py`.
- Create `tools/wrap_mbr.py`: deterministic MBR wrapper for filesystem payload images.
- Modify `tools/check_ext3_linux_compat.py` and `tools/check_ext3_journal_activity.py`: read ext3 data from `sda1` partition offset.
- Modify `Makefile`: build partitioned `disk.img` and `dufs.img`, add `lsblk`, add `sysfs` and `blkdev_part` objects.
- Modify `kernel/fs/fs.c` and `kernel/fs/fs.h`: let DUFS select `sda1`/`sdb1`; default to `sdb1` for the secondary DUFS mount.
- Modify `kernel/fs/ext3.c` and `kernel/fs/ext3.h`: let ext3 mount from `sda1`.
- Modify `kernel/fs/vfs.h` and `kernel/fs/vfs.c`: add block-node type, sysfs mount kind, mount source/fstype tracking, and sysfs dispatch.
- Create `kernel/fs/sysfs.h` and `kernel/fs/sysfs.c`: narrow `/sys/block` synthetic filesystem.
- Modify `kernel/fs/procfs.h` and `kernel/fs/procfs.c`: render dynamic `/proc/mounts`.
- Modify `kernel/proc/process.h` and `kernel/proc/syscall.c`: represent block-device file descriptors and support read-only byte reads for filesystem probing.
- Modify `kernel/kernel.c`: scan partitions and mount `/sys`.
- Create `user/lsblk.cpp`: parse `/sys/block`, associate mountpoints from `/proc/mounts`, probe filesystems, and print Linux-like tables.
- Modify `user/Makefile`, root `Makefile`, and `README.md`: include `lsblk` in builds and document the partitioned disk shape.

---

### Task 1: Add Block Registry Metadata And Tests

**Files:**
- Modify: `kernel/drivers/blkdev.h`
- Modify: `kernel/drivers/blkdev.c`
- Create: `kernel/test/test_blkdev.c`
- Modify: `kernel/test/ktest.h`
- Modify: `kernel/test/ktest.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing KTEST coverage for enumerable block devices**

Add `kernel/test/test_blkdev.c` with an initial suite that expects two named disk records to be enumerable:

```c
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_blkdev.c - block-device registry tests.
 */

#include "ktest.h"
#include "blkdev.h"
#include "kstring.h"

static int null_read(uint32_t lba, uint8_t *buf)
{
    (void)lba;
    if (buf)
        k_memset(buf, 0, BLKDEV_SECTOR_SIZE);
    return 0;
}

static int null_write(uint32_t lba, const uint8_t *buf)
{
    (void)lba;
    (void)buf;
    return 0;
}

static const blkdev_ops_t null_ops = {
    .read_sector = null_read,
    .write_sector = null_write,
};

static void test_blkdev_enumerates_disks(ktest_case_t *tc)
{
    blkdev_info_t info;

    blkdev_reset();
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops), 0u);
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register_disk("sdb", 8u, 16u, 200u, &null_ops), 0u);

    KTEST_EXPECT_EQ(tc, blkdev_count(), 2u);
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(0u, &info), 0u);
    KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sda") == 0);
    KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_DISK);
    KTEST_EXPECT_EQ(tc, info.major, 8u);
    KTEST_EXPECT_EQ(tc, info.minor, 0u);
    KTEST_EXPECT_EQ(tc, info.sectors, 100u);

    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
    KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sdb") == 0);
    KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_DISK);
    KTEST_EXPECT_EQ(tc, info.major, 8u);
    KTEST_EXPECT_EQ(tc, info.minor, 16u);
    KTEST_EXPECT_EQ(tc, info.sectors, 200u);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_blkdev_enumerates_disks),
};

static ktest_suite_t suite = KTEST_SUITE("blkdev", cases);

ktest_suite_t *ktest_suite_blkdev(void) { return &suite; }
```

- [ ] **Step 2: Register the failing suite**

Add this prototype to `kernel/test/ktest.h`:

```c
ktest_suite_t *ktest_suite_blkdev(void);
```

Add this line near the other `run_and_tally` calls in `kernel/test/ktest.c`:

```c
run_and_tally(ktest_suite_blkdev(),  &total_pass, &total_fail);
```

Add `kernel/test/test_blkdev.o` to `KTOBJS` in `Makefile`:

```make
KTOBJS  = kernel/test/ktest.o \
           kernel/test/test_pmm.o \
           kernel/test/test_kheap.o \
           kernel/test/test_vfs.o \
           kernel/test/test_process.o \
           kernel/test/test_sched.o \
           kernel/test/test_fs.o \
           kernel/test/test_uaccess.o \
           kernel/test/test_desktop.o \
           kernel/test/test_blkdev.o
```

- [ ] **Step 3: Run the failing test build**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails because `blkdev_info_t`, `blkdev_reset`, `blkdev_register_disk`, `blkdev_count`, `blkdev_info_at`, and `BLKDEV_KIND_DISK` do not exist yet.

- [ ] **Step 4: Add the public block metadata API**

Replace the registry declarations in `kernel/drivers/blkdev.h` with this expanded API while preserving `blkdev_get`:

```c
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
const blkdev_ops_t *blkdev_get(const char *name);
uint32_t blkdev_count(void);
int blkdev_info_at(uint32_t index, blkdev_info_t *out);
int blkdev_find_index(const char *name);
```

- [ ] **Step 5: Implement the enumerable registry**

Update `kernel/drivers/blkdev.c` around a single table:

```c
typedef struct {
    blkdev_info_t info;
    const blkdev_ops_t *ops;
} blkdev_entry_t;

static blkdev_entry_t blkdev_table[BLKDEV_MAX];

static int blkdev_name_equals(const char *a, const char *b)
{
    return k_strcmp(a, b) == 0;
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

    if (!name || !ops || sectors == 0 || blkdev_find_index(name) >= 0)
        return -1;
    idx = blkdev_alloc_slot();
    if (idx < 0)
        return -1;

    k_strncpy(blkdev_table[idx].info.name, name, BLKDEV_NAME_MAX - 1);
    blkdev_table[idx].info.name[BLKDEV_NAME_MAX - 1] = '\0';
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
    return idx;
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
```

Add `#include "kstring.h"` to `kernel/drivers/blkdev.c`.

- [ ] **Step 6: Run the test build**

Run:

```bash
make KTEST=1 kernel
```

Expected: build fails only because `blkdev_register_part` is declared but not defined.

- [ ] **Step 7: Add a stub partition registration function**

Add this temporary implementation in `kernel/drivers/blkdev.c`; Task 2 replaces the ops behavior with real partition wrappers:

```c
int blkdev_register_part(const char *name, uint32_t parent_index,
                         uint32_t partition_number, uint32_t start_sector,
                         uint32_t sectors)
{
    int idx;
    const blkdev_entry_t *parent;

    if (!name || sectors == 0 || parent_index >= BLKDEV_MAX ||
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
    k_strncpy(blkdev_table[idx].info.name, name, BLKDEV_NAME_MAX - 1);
    blkdev_table[idx].info.name[BLKDEV_NAME_MAX - 1] = '\0';
    blkdev_table[idx].info.kind = BLKDEV_KIND_PART;
    blkdev_table[idx].info.sector_size = BLKDEV_SECTOR_SIZE;
    blkdev_table[idx].info.sectors = sectors;
    blkdev_table[idx].info.readonly = parent->info.readonly;
    blkdev_table[idx].info.major = parent->info.major;
    blkdev_table[idx].info.minor = parent->info.minor + partition_number;
    blkdev_table[idx].info.parent_index = parent_index;
    blkdev_table[idx].info.start_sector = start_sector;
    blkdev_table[idx].info.partition_number = partition_number;
    blkdev_table[idx].ops = parent->ops;
    return idx;
}
```

- [ ] **Step 8: Verify tests pass**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds.

- [ ] **Step 9: Commit**

```bash
git add kernel/drivers/blkdev.h kernel/drivers/blkdev.c kernel/test/test_blkdev.c kernel/test/ktest.h kernel/test/ktest.c Makefile
git commit -m "feat: add enumerable block device registry"
```

---

### Task 2: Add Partition Ops And MBR Scanning

**Files:**
- Create: `kernel/drivers/blkdev_part.c`
- Modify: `kernel/drivers/blkdev.h`
- Modify: `kernel/drivers/blkdev.c`
- Modify: `kernel/test/test_blkdev.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing tests for partition offset translation and MBR parsing**

Append these helpers and tests to `kernel/test/test_blkdev.c`:

```c
static uint32_t last_read_lba;
static uint32_t last_write_lba;
static uint8_t fake_disk[16][BLKDEV_SECTOR_SIZE];

static int fake_read(uint32_t lba, uint8_t *buf)
{
    last_read_lba = lba;
    if (!buf || lba >= 16)
        return -1;
    k_memcpy(buf, fake_disk[lba], BLKDEV_SECTOR_SIZE);
    return 0;
}

static int fake_write(uint32_t lba, const uint8_t *buf)
{
    last_write_lba = lba;
    if (!buf || lba >= 16)
        return -1;
    k_memcpy(fake_disk[lba], buf, BLKDEV_SECTOR_SIZE);
    return 0;
}

static const blkdev_ops_t fake_ops = {
    .read_sector = fake_read,
    .write_sector = fake_write,
};

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_mbr_entry(uint8_t *mbr, uint32_t slot, uint8_t type,
                            uint32_t start, uint32_t sectors)
{
    uint8_t *ent = mbr + 446u + slot * 16u;
    ent[4] = type;
    put_le32(ent + 8, start);
    put_le32(ent + 12, sectors);
}

static void test_blkdev_partition_translates_lba(ktest_case_t *tc)
{
    uint8_t buf[BLKDEV_SECTOR_SIZE];
    int disk;

    blkdev_reset();
    k_memset(fake_disk, 0, sizeof(fake_disk));
    disk = blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops);
    KTEST_ASSERT_TRUE(tc, disk >= 0);
    KTEST_ASSERT_TRUE(tc, blkdev_register_part("sda1", (uint32_t)disk, 1u, 4u, 8u) >= 0);

    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_get("sda1")->read_sector(2u, buf), 0u);
    KTEST_EXPECT_EQ(tc, last_read_lba, 6u);
    KTEST_EXPECT_TRUE(tc, blkdev_get("sda1")->read_sector(8u, buf) < 0);
}

static void test_blkdev_scan_mbr_registers_primary_partition(ktest_case_t *tc)
{
    blkdev_info_t info;
    int disk;

    blkdev_reset();
    k_memset(fake_disk, 0, sizeof(fake_disk));
    fake_disk[0][510] = 0x55;
    fake_disk[0][511] = 0xAA;
    write_mbr_entry(fake_disk[0], 0u, 0x83u, 1u, 7u);
    disk = blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops);
    KTEST_ASSERT_TRUE(tc, disk >= 0);

    KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_scan_mbr((uint32_t)disk), 1u);
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
    KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sda1") == 0);
    KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_PART);
    KTEST_EXPECT_EQ(tc, info.start_sector, 1u);
    KTEST_EXPECT_EQ(tc, info.sectors, 7u);
}
```

Add both new cases to the `cases[]` array:

```c
KTEST_CASE(test_blkdev_partition_translates_lba),
KTEST_CASE(test_blkdev_scan_mbr_registers_primary_partition),
```

- [ ] **Step 2: Run tests to verify failure**

Run:

```bash
make KTEST=1 kernel
```

Expected: build fails because `blkdev_scan_mbr` is not declared or defined, and partition ops still point directly at the parent.

- [ ] **Step 3: Declare partition scanning**

Add to `kernel/drivers/blkdev.h`:

```c
int blkdev_scan_mbr(uint32_t disk_index);
const blkdev_ops_t *blkdev_ops_at(uint32_t index);
```

- [ ] **Step 4: Expose ops by table index**

Add to `kernel/drivers/blkdev.c`:

```c
const blkdev_ops_t *blkdev_ops_at(uint32_t index)
{
    if (index >= BLKDEV_MAX || blkdev_table[index].info.name[0] == '\0')
        return 0;
    return blkdev_table[index].ops;
}
```

- [ ] **Step 5: Implement partition wrappers**

Create `kernel/drivers/blkdev_part.c`:

```c
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * blkdev_part.c - MBR partition devices over parent block devices.
 */

#include "blkdev.h"
#include "kprintf.h"
#include "kstring.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int part_translate(uint32_t part_index, uint32_t lba, blkdev_info_t *info_out,
                          const blkdev_ops_t **parent_ops_out,
                          uint32_t *parent_lba_out)
{
    blkdev_info_t info;

    if (blkdev_info_at(part_index, &info) != 0 ||
        info.kind != BLKDEV_KIND_PART ||
        lba >= info.sectors)
        return -1;
    *parent_ops_out = blkdev_ops_at(info.parent_index);
    if (!*parent_ops_out)
        return -1;
    *parent_lba_out = info.start_sector + lba;
    if (info_out)
        *info_out = info;
    return 0;
}

static int part_read_sector_for(uint32_t part_index, uint32_t lba, uint8_t *buf)
{
    const blkdev_ops_t *parent_ops;
    uint32_t parent_lba;

    if (part_translate(part_index, lba, 0, &parent_ops, &parent_lba) != 0)
        return -1;
    return parent_ops->read_sector(parent_lba, buf);
}

static int part_write_sector_for(uint32_t part_index, uint32_t lba, const uint8_t *buf)
{
    const blkdev_ops_t *parent_ops;
    uint32_t parent_lba;

    if (part_translate(part_index, lba, 0, &parent_ops, &parent_lba) != 0)
        return -1;
    return parent_ops->write_sector(parent_lba, buf);
}

#define DEFINE_PART_OPS(n) \
static int part##n##_read_sector(uint32_t lba, uint8_t *buf) \
{ \
    return part_read_sector_for((uint32_t)(n), lba, buf); \
} \
static int part##n##_write_sector(uint32_t lba, const uint8_t *buf) \
{ \
    return part_write_sector_for((uint32_t)(n), lba, buf); \
} \
static const blkdev_ops_t part##n##_ops = { \
    .read_sector = part##n##_read_sector, \
    .write_sector = part##n##_write_sector, \
};

DEFINE_PART_OPS(0)
DEFINE_PART_OPS(1)
DEFINE_PART_OPS(2)
DEFINE_PART_OPS(3)
DEFINE_PART_OPS(4)
DEFINE_PART_OPS(5)
DEFINE_PART_OPS(6)
DEFINE_PART_OPS(7)
DEFINE_PART_OPS(8)
DEFINE_PART_OPS(9)
DEFINE_PART_OPS(10)
DEFINE_PART_OPS(11)
DEFINE_PART_OPS(12)
DEFINE_PART_OPS(13)
DEFINE_PART_OPS(14)
DEFINE_PART_OPS(15)

const blkdev_ops_t *blkdev_part_ops_for_index(uint32_t index)
{
    static const blkdev_ops_t *ops_by_index[BLKDEV_MAX] = {
        &part0_ops, &part1_ops, &part2_ops, &part3_ops,
        &part4_ops, &part5_ops, &part6_ops, &part7_ops,
        &part8_ops, &part9_ops, &part10_ops, &part11_ops,
        &part12_ops, &part13_ops, &part14_ops, &part15_ops,
    };

    if (index >= BLKDEV_MAX)
        return 0;
    return ops_by_index[index];
}

int blkdev_scan_mbr(uint32_t disk_index)
{
    blkdev_info_t disk;
    const blkdev_ops_t *ops;
    uint8_t mbr[BLKDEV_SECTOR_SIZE];
    int registered = 0;

    if (blkdev_info_at(disk_index, &disk) != 0 || disk.kind != BLKDEV_KIND_DISK)
        return -1;
    ops = blkdev_ops_at(disk_index);
    if (!ops || ops->read_sector(0, mbr) != 0)
        return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return 0;

    for (uint32_t slot = 0; slot < 4; slot++) {
        uint8_t *ent = mbr + 446u + slot * 16u;
        uint8_t type = ent[4];
        uint32_t start = le32(ent + 8);
        uint32_t sectors = le32(ent + 12);
        char name[BLKDEV_NAME_MAX];

        if (type == 0 || sectors == 0)
            continue;
        if (start >= disk.sectors || sectors > disk.sectors - start)
            continue;
        k_snprintf(name, sizeof(name), "%s%u", disk.name, slot + 1u);
        if (blkdev_register_part(name, disk_index, slot + 1u, start, sectors) >= 0)
            registered++;
    }
    return registered;
}
```

Each partition gets an ops table whose function body closes over a fixed table
index at compile time. Do not use a shared mutable "current partition" global;
ext3 and DUFS keep block-device ops pointers across mounts, so shared mutable
identity would make the last lookup affect earlier mounts.

- [ ] **Step 6: Wire partition ops into the registry**

Declare in `kernel/drivers/blkdev.c`:

```c
const blkdev_ops_t *blkdev_part_ops_for_index(uint32_t index);
```

In `blkdev_register_part`, set:

```c
blkdev_table[idx].ops = blkdev_part_ops_for_index((uint32_t)idx);
```

instead of inheriting `parent->ops`.

- [ ] **Step 7: Add the new object to the kernel build**

Add `kernel/drivers/blkdev_part.o` next to `kernel/drivers/blkdev.o` in `KOBJS` and `KOBJS_VGA` inherits it automatically through the filter:

```make
kernel/drivers/blkdev.o kernel/drivers/blkdev_part.o
```

- [ ] **Step 8: Verify KTEST build**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds.

- [ ] **Step 9: Run headless tests**

Run:

```bash
make check
```

Expected: `KTEST: SUMMARY ... fail=0`.

- [ ] **Step 10: Commit**

```bash
git add kernel/drivers/blkdev.h kernel/drivers/blkdev.c kernel/drivers/blkdev_part.c kernel/test/test_blkdev.c Makefile
git commit -m "feat: scan MBR partitions"
```

---

### Task 3: Register ATA Disks As `sda` And `sdb`

**Files:**
- Modify: `kernel/drivers/ata.c`
- Modify: `kernel/drivers/ata.h`
- Modify: `kernel/kernel.c`
- Modify: `kernel/test/test_fs.c`

- [ ] **Step 1: Update ATA registration names**

Change `ata_register` in `kernel/drivers/ata.c`:

```c
void ata_register(void) {
    blkdev_register_disk("sda", 8u, 0u, 102400u, &ata_master_ops);
    blkdev_register_disk("sdb", 8u, 16u, 102400u, &ata_slave_ops);
}
```

Keep the sector count tied to `DISK_SECTORS` for this implementation by using `102400u`, matching the current Makefile default.

- [ ] **Step 2: Scan partitions during boot**

In `kernel/kernel.c`, immediately after `ata_register()` is called, add:

```c
int sda_idx = blkdev_find_index("sda");
int sdb_idx = blkdev_find_index("sdb");
if (sda_idx >= 0)
    blkdev_scan_mbr((uint32_t)sda_idx);
if (sdb_idx >= 0)
    blkdev_scan_mbr((uint32_t)sdb_idx);
```

Add `#include "blkdev.h"` if the file does not already include it.

- [ ] **Step 3: Update DUFS unit test device name**

In `kernel/test/test_fs.c`, change:

```c
if (dufs_use_device("hd1") != 0)
```

to:

```c
if (dufs_use_device("sdb1") != 0)
```

This will fail until Task 5 extends DUFS device selection. Keep it in this task if you want the failure visible before the filesystem binding work.

- [ ] **Step 4: Run the expected failing test build**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds, but full `make check` may fail until image wrapping and DUFS binding land.

- [ ] **Step 5: Commit**

```bash
git add kernel/drivers/ata.c kernel/drivers/ata.h kernel/kernel.c kernel/test/test_fs.c
git commit -m "feat: register ATA disks as sd devices"
```

---

### Task 4: Build Partitioned Disk Images

**Files:**
- Create: `tools/wrap_mbr.py`
- Modify: `Makefile`
- Modify: `tools/check_ext3_linux_compat.py`
- Modify: `tools/check_ext3_journal_activity.py`

- [ ] **Step 1: Create a failing host check for MBR wrapping**

Create `tools/wrap_mbr.py` first with only argument validation and a deliberate failure:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import sys

def main():
    print("wrap_mbr.py not implemented", file=sys.stderr)
    return 1

if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Run the expected failing wrapper command**

Run:

```bash
python3 tools/wrap_mbr.py /tmp/drunix.fs /tmp/drunix.disk 2048 102400 0x83
```

Expected: exits nonzero with `wrap_mbr.py not implemented`.

- [ ] **Step 3: Implement deterministic MBR wrapping**

Replace `tools/wrap_mbr.py` with:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import struct
import sys

SECTOR = 512

def usage():
    print("usage: wrap_mbr.py <payload> <output> <start-sector> <total-sectors> <type>", file=sys.stderr)
    return 1

def main():
    if len(sys.argv) != 6:
        return usage()
    payload, output = sys.argv[1], sys.argv[2]
    start = int(sys.argv[3], 0)
    total = int(sys.argv[4], 0)
    ptype = int(sys.argv[5], 0)
    if start <= 0 or total <= start or not (0 <= ptype <= 0xFF):
        return usage()

    with open(payload, "rb") as f:
        data = f.read()
    part_sectors = (len(data) + SECTOR - 1) // SECTOR
    if start + part_sectors > total:
        raise SystemExit("payload does not fit in partition")

    image = bytearray(total * SECTOR)
    mbr = image[:SECTOR]
    entry = 446
    mbr[entry + 4] = ptype
    struct.pack_into("<I", mbr, entry + 8, start)
    struct.pack_into("<I", mbr, entry + 12, part_sectors)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    off = start * SECTOR
    image[off:off + len(data)] = data
    with open(output, "wb") as f:
        f.write(image)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Verify the wrapper produces an MBR**

Run:

```bash
python3 tools/mkext3.py /tmp/drunix-ext3.fs 100352 tools/hello.txt hello.txt
python3 tools/wrap_mbr.py /tmp/drunix-ext3.fs /tmp/drunix-ext3.disk 2048 102400 0x83
python3 -c 'import sys; d=open("/tmp/drunix-ext3.disk","rb").read(512); sys.exit(0 if d[510:512] == b"\x55\xaa" else 1)'
```

Expected: all commands exit 0.

- [ ] **Step 5: Update Makefile image rules**

Change the image rules to build payloads and wrap them:

```make
PARTITION_START ?= 2048
FS_SECTORS := $(shell expr $(DISK_SECTORS) - $(PARTITION_START))

ifeq ($(ROOT_FS),dufs)
disk.fs: $(USER_BINS) tools/hello.txt tools/readme.txt tools/mkfs.py
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS) $(DISK_FILES)
disk.img: disk.fs tools/wrap_mbr.py
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA
else
disk.fs: $(USER_BINS) tools/hello.txt tools/readme.txt tools/mkext3.py
	$(PYTHON) tools/mkext3.py $@ $(FS_SECTORS) $(DISK_FILES)
disk.img: disk.fs tools/wrap_mbr.py
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0x83
endif

dufs.fs: tools/mkfs.py
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS)

dufs.img: dufs.fs tools/wrap_mbr.py
	$(PYTHON) tools/wrap_mbr.py dufs.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA
```

Add `disk.fs dufs.fs` to the `clean` removal list so stale payloads do not linger:

```make
	$(RM) *.elf core.* disk.img dufs.img disk.fs dufs.fs $(TEST_IMAGES) os.iso $(ISO_KERNEL) $(ISO_KERNEL_VGA) iso/boot/grub/grub.cfg "$(PDF)" "$(EPUB)" $(SENTINELS)
```

- [ ] **Step 6: Update host ext3 validators to read the partition**

In `tools/check_ext3_linux_compat.py`, add:

```python
def mbr_partition_offset(path):
    with open(path, "rb") as f:
        mbr = f.read(512)
    if len(mbr) != 512 or mbr[510:512] != b"\x55\xaa":
        return 0
    start = int.from_bytes(mbr[446 + 8:446 + 12], "little")
    return start * 512
```

Use this offset before every ext3 superblock read:

```python
base = mbr_partition_offset(image_path)
f.seek(base + 1024)
```

In `tools/check_ext3_journal_activity.py`, add the same `mbr_partition_offset()` helper and add `base` to ext3 block reads.

- [ ] **Step 7: Verify image build and host check**

Run:

```bash
make clean
make disk
python3 tools/check_ext3_linux_compat.py disk.img
```

Expected: `make disk` succeeds and the ext3 compatibility check succeeds against `sda1`.

- [ ] **Step 8: Commit**

```bash
git add tools/wrap_mbr.py tools/check_ext3_linux_compat.py tools/check_ext3_journal_activity.py Makefile
git commit -m "feat: build MBR partitioned disk images"
```

---

### Task 5: Mount Filesystems From Partition Devices

**Files:**
- Modify: `kernel/fs/fs.h`
- Modify: `kernel/fs/fs.c`
- Modify: `kernel/fs/ext3.h`
- Modify: `kernel/fs/ext3.c`
- Modify: `kernel/kernel.c`
- Modify: `kernel/test/test_fs.c`

- [ ] **Step 1: Write failing tests for DUFS device selection**

In `kernel/test/test_fs.c`, keep `fs_test_init()` using `sdb1`. Add this explicit test:

```c
static void test_fs_accepts_sdb1_device_name(ktest_case_t *tc)
{
    KTEST_EXPECT_EQ(tc, (uint32_t)dufs_use_device("sdb1"), 0u);
}
```

Add it to the `cases[]` array before `test_fs_init_ok`.

- [ ] **Step 2: Run the expected failing test**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds; `make check` fails because `dufs_use_device("sdb1")` returns `-1`.

- [ ] **Step 3: Extend DUFS mount states to `sda1` and `sdb1`**

In `kernel/fs/fs.c`, replace the old mount state declarations:

```c
static dufs_mount_t g_dufs_sda1 = { .blkdev_name = "sda1" };
static dufs_mount_t g_dufs_sdb1 = { .blkdev_name = "sdb1" };
static dufs_mount_t *g_dufs = &g_dufs_sda1;
```

Update `dufs_use_device`:

```c
int dufs_use_device(const char *blkdev_name)
{
    if (!blkdev_name)
        return -1;
    if (k_strcmp(blkdev_name, "sda1") == 0) {
        dufs_select(&g_dufs_sda1);
        return 0;
    }
    if (k_strcmp(blkdev_name, "sdb1") == 0) {
        dufs_select(&g_dufs_sdb1);
        return 0;
    }
    return -1;
}
```

Update `dufs_register()` to pick the right default:

```c
const fs_ops_t *ops = (k_strcmp(DRUNIX_ROOT_FS, "dufs") == 0)
                          ? &dufs_sda1_ops
                          : &dufs_sdb1_ops;
vfs_register("dufs", ops);
```

Rename the corresponding `dufs_hd0_ops`/`dufs_hd1_ops` objects to `dufs_sda1_ops`/`dufs_sdb1_ops` and set their contexts to the new mount states.

- [ ] **Step 4: Make ext3 mount from `sda1`**

In `kernel/fs/ext3.c`, change:

```c
g_dev = blkdev_get("hd0");
```

to:

```c
g_dev = blkdev_get("sda1");
```

Do not add an ext3 device-selection API in this first pass. The selected boot
path mounts the single ext3 root from `sda1`.

- [ ] **Step 5: Verify KTEST build and headless tests**

Run:

```bash
make KTEST=1 kernel
make check
```

Expected: both succeed after partitioned images are present.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/fs.h kernel/fs/fs.c kernel/fs/ext3.h kernel/fs/ext3.c kernel/kernel.c kernel/test/test_fs.c
git commit -m "feat: mount filesystems from sd partitions"
```

---

### Task 6: Add Devfs Block Nodes

**Files:**
- Modify: `kernel/fs/vfs.h`
- Modify: `kernel/fs/vfs.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/test/test_vfs.c`

- [ ] **Step 1: Write failing VFS tests for `/dev/sd*` nodes**

In `kernel/test/test_vfs.c`, add:

```c
static void test_devfs_exposes_block_devices(ktest_case_t *tc)
{
    vfs_node_t node;
    char buf[256];
    int n;

    KTEST_ASSERT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);
    KTEST_ASSERT_EQ(tc, (uint32_t)vfs_resolve("/dev/sda", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, VFS_NODE_BLOCKDEV);
    KTEST_EXPECT_TRUE(tc, k_strcmp(node.dev_name, "sda") == 0);

    KTEST_ASSERT_EQ(tc, (uint32_t)vfs_resolve("/dev/sda1", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, VFS_NODE_BLOCKDEV);
    KTEST_EXPECT_TRUE(tc, k_strcmp(node.dev_name, "sda1") == 0);

    n = vfs_getdents("/dev", buf, sizeof(buf));
    KTEST_ASSERT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sda"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sda1"));
}
```

Add it to the VFS `cases[]` array.

- [ ] **Step 2: Run the expected failing test build**

Run:

```bash
make KTEST=1 kernel
```

Expected: build fails because `VFS_NODE_BLOCKDEV` is not defined.

- [ ] **Step 3: Add a block-device VFS node type**

In `kernel/fs/vfs.h`, add:

```c
VFS_NODE_BLOCKDEV = 7,
```

In the comment over `vfs_node_t`, add:

```c
 * For VFS_NODE_BLOCKDEV, dev_name names the block device.
```

- [ ] **Step 4: Generate devfs entries from block registry**

In `kernel/fs/vfs.c`, include `blkdev.h`:

```c
#include "blkdev.h"
```

In `devfs_fill_node`, after static character device lookup, add:

```c
if (blkdev_find_index(relpath) >= 0) {
    node_out->type = VFS_NODE_BLOCKDEV;
    k_strncpy(node_out->dev_name, relpath, VFS_DEV_NAME_MAX - 1);
    node_out->dev_name[VFS_DEV_NAME_MAX - 1] = '\0';
    return 0;
}
```

In `devfs_getdents`, append block-device names after static entries:

```c
for (uint32_t i = 0; i < blkdev_count(); i++) {
    blkdev_info_t info;
    if (blkdev_info_at(i, &info) == 0) {
        if (vfs_append_dirent(buf, bufsz, &written, info.name, 0) != 0)
            break;
    }
}
```

- [ ] **Step 5: Make block devices openable and readable**

In `kernel/proc/process.h`, add:

```c
FD_TYPE_BLOCKDEV = 8,
```

and a union field:

```c
struct {
    char name[VFS_DEV_NAME_MAX];
    uint32_t offset;
} blockdev;
```

In `kernel/proc/syscall.c`, where `SYS_OPEN` handles `VFS_NODE_CHARDEV` and `VFS_NODE_TTY`, add a case for `VFS_NODE_BLOCKDEV` that stores the name and marks the descriptor read-only:

```c
fh->type = FD_TYPE_BLOCKDEV;
fh->writable = 0;
fh->u.blockdev.offset = 0;
k_strncpy(fh->u.blockdev.name, node.dev_name, sizeof(fh->u.blockdev.name) - 1);
fh->u.blockdev.name[sizeof(fh->u.blockdev.name) - 1] = '\0';
return fd;
```

In read/write/close switch statements, make `FD_TYPE_BLOCKDEV` close normally,
return `-1` for `write`, and support read-only byte reads for filesystem
probing. Use a 512-byte sector buffer, read the sector containing the current
descriptor offset, copy the requested byte range to the user buffer, and advance
the offset.

Use this helper shape in `kernel/proc/syscall.c`:

```c
static int read_blockdev_fd(file_handle_t *fh, uint32_t user_buf, uint32_t count)
{
    const blkdev_ops_t *ops;
    uint8_t sector[BLKDEV_SECTOR_SIZE];
    uint32_t done = 0;
    process_t *cur = sched_current();

    if (!fh || fh->type != FD_TYPE_BLOCKDEV || user_buf == 0)
        return -1;
    ops = blkdev_get(fh->u.blockdev.name);
    if (!ops)
        return -1;

    while (done < count) {
        uint32_t lba = fh->u.blockdev.offset / BLKDEV_SECTOR_SIZE;
        uint32_t off = fh->u.blockdev.offset % BLKDEV_SECTOR_SIZE;
        uint32_t n = BLKDEV_SECTOR_SIZE - off;

        if (n > count - done)
            n = count - done;
        if (ops->read_sector(lba, sector) != 0)
            return done ? (int)done : -1;
        if (uaccess_copy_to_user(cur, user_buf + done, sector + off, n) != 0)
            return done ? (int)done : -1;
        fh->u.blockdev.offset += n;
        done += n;
    }

    return (int)done;
}
```

Call `read_blockdev_fd(fh, ecx, edx)` from the existing `SYS_READ` fd-type
dispatch. If the dispatch has already normalized argument names, pass the
equivalent user buffer address and byte count variables.

- [ ] **Step 6: Verify tests**

Run:

```bash
make KTEST=1 kernel
make check
```

Expected: VFS tests pass and KTEST summary reports `fail=0`.

- [ ] **Step 7: Commit**

```bash
git add kernel/fs/vfs.h kernel/fs/vfs.c kernel/proc/process.h kernel/proc/syscall.c kernel/test/test_vfs.c
git commit -m "feat: expose block devices in devfs"
```

---

### Task 7: Add Minimal `/sys/block`

**Files:**
- Create: `kernel/fs/sysfs.h`
- Create: `kernel/fs/sysfs.c`
- Modify: `kernel/fs/vfs.c`
- Modify: `kernel/fs/vfs.h`
- Modify: `kernel/test/test_vfs.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing sysfs VFS tests**

In `kernel/test/test_vfs.c`, add:

```c
static int setup_mount_tree_with_sys(void)
{
    if (setup_mount_tree() != 0)
        return -1;
    if (vfs_mount("/sys", "sysfs") != 0)
        return -1;
    return 0;
}

static int read_text_path(const char *path, char *buf, uint32_t cap)
{
    vfs_file_ref_t ref;
    uint32_t size;
    int n;

    if (vfs_open_file(path, &ref, &size) != 0)
        return -1;
    n = vfs_read(ref, 0, (uint8_t *)buf, cap - 1u);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static void test_sysfs_block_tree(ktest_case_t *tc)
{
    char dents[256];
    char text[64];
    int n;

    blkdev_reset();
    KTEST_ASSERT_TRUE(tc, blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops) >= 0);
    KTEST_ASSERT_TRUE(tc, blkdev_register_part("sda1", 0u, 1u, 10u, 90u) >= 0);
    KTEST_ASSERT_EQ(tc, (uint32_t)setup_mount_tree_with_sys(), 0u);

    n = vfs_getdents("/sys/block", dents, sizeof(dents));
    KTEST_ASSERT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(dents, n, "sda/"));

    KTEST_ASSERT_TRUE(tc, read_text_path("/sys/block/sda/size", text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "100\n") == 0);
    KTEST_ASSERT_TRUE(tc, read_text_path("/sys/block/sda/sda1/start", text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "10\n") == 0);
}
```

Add the test to `cases[]`. Add `#include "blkdev.h"` to the test file if not already present.

- [ ] **Step 2: Run the expected failing build**

Run:

```bash
make KTEST=1 kernel
```

Expected: build or test fails because `sysfs` mount kind and handlers do not exist.

- [ ] **Step 3: Add sysfs interface files**

Create `kernel/fs/sysfs.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SYSFS_H
#define SYSFS_H

#include "vfs.h"
#include <stdint.h>

int sysfs_fill_node(const char *relpath, vfs_node_t *node_out);
int sysfs_stat(const char *relpath, vfs_stat_t *st);
int sysfs_getdents(const char *relpath, char *buf, uint32_t bufsz);
int sysfs_file_size(uint32_t index, uint32_t *size_out);
int sysfs_read_file(uint32_t index, uint32_t offset, char *buf, uint32_t count);

#endif
```

Create `kernel/fs/sysfs.c` with the same rendering pattern as `procfs.c`: parse paths under `block`, map each leaf file to a synthetic index, render text into a fixed buffer, and return directory entries using null-terminated names.

Use these leaf names and formats:

```c
/* disk and partition */
"size"      -> "%u\n", info.sectors
"dev"       -> "%u:%u\n", info.major, info.minor
"type"      -> "%s\n", info.kind == BLKDEV_KIND_DISK ? "disk" : "part"
/* partition only */
"partition" -> "%u\n", info.partition_number
"start"     -> "%u\n", info.start_sector
```

- [ ] **Step 4: Wire sysfs into VFS**

In `kernel/fs/vfs.c`, include `sysfs.h`:

```c
#include "sysfs.h"
```

Add mount kind:

```c
VFS_MOUNT_KIND_SYSFS = 4,
```

In `vfs_lookup_fs`:

```c
if (k_strcmp(name, "sysfs") == 0) {
    if (kind_out)
        *kind_out = VFS_MOUNT_KIND_SYSFS;
    return 0;
}
```

In the mount validation condition, allow sysfs:

```c
kind != VFS_MOUNT_KIND_SYSFS
```

In `vfs_resolve`, dispatch:

```c
if (mnt->kind == VFS_MOUNT_KIND_SYSFS) {
    int rc = sysfs_fill_node(rel, node_out);
    kfree(norm);
    return rc;
}
```

In `vfs_getdents`, dispatch:

```c
} else if (mnt->kind == VFS_MOUNT_KIND_SYSFS) {
    n = sysfs_getdents(rel, buf, bufsz);
    if (n < 0) {
        kfree(norm);
        return -1;
    }
    written = (uint32_t)n;
}
```

In `vfs_stat`, dispatch to `sysfs_stat` in the same place as procfs.

- [ ] **Step 5: Make sysfs files readable through existing procfile path**

Add a new VFS node type:

```c
VFS_NODE_SYSFILE = 8,
```

Add `sys_index` to `vfs_node_t`:

```c
uint32_t sys_index;
```

Update initialization sites in `vfs_resolve`, `devfs_fill_node`, and procfs fill paths to set `sys_index = 0`.

In `process.h`, add:

```c
FD_TYPE_SYSFILE = 9,
```

and:

```c
struct {
    uint32_t index;
} sys;
```

In `syscall.c`, mirror the existing `FD_TYPE_PROCFILE` open/read/seek/stat behavior using `sysfs_file_size` and `sysfs_read_file`.

- [ ] **Step 6: Add sysfs object to the build**

In `Makefile`, add `kernel/fs/sysfs.o` to `KOBJS`.

- [ ] **Step 7: Verify tests**

Run:

```bash
make KTEST=1 kernel
make check
```

Expected: sysfs tests pass.

- [ ] **Step 8: Commit**

```bash
git add kernel/fs/sysfs.h kernel/fs/sysfs.c kernel/fs/vfs.h kernel/fs/vfs.c kernel/proc/process.h kernel/proc/syscall.c kernel/test/test_vfs.c Makefile
git commit -m "feat: add sysfs block metadata"
```

---

### Task 8: Render Accurate `/proc/mounts`

**Files:**
- Modify: `kernel/fs/vfs.h`
- Modify: `kernel/fs/vfs.c`
- Modify: `kernel/fs/procfs.c`
- Modify: `kernel/fs/procfs.h`
- Modify: `kernel/test/test_vfs.c`

- [ ] **Step 1: Write failing test for dynamic mounts**

In `kernel/test/test_vfs.c`, add:

```c
static void test_procfs_mounts_reports_sources(ktest_case_t *tc)
{
    char buf[512];
    int n;

    KTEST_ASSERT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    n = procfs_read_file(PROCFS_FILE_MOUNTS, 0u, 0u, 0u, buf, sizeof(buf) - 1u);
    KTEST_ASSERT_TRUE(tc, n > 0);
    buf[n] = '\0';
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "rootfs / root rw 0 0") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "devfs /dev devfs rw,nosuid 0 0") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0") != 0);
}
```

Add to `cases[]`.

- [ ] **Step 2: Run expected failing test**

Run:

```bash
make KTEST=1 kernel
make check
```

Expected: `test_procfs_mounts_reports_sources` fails because `/proc/mounts` is hard-coded.

- [ ] **Step 3: Track source and fstype in VFS mount records**

In `kernel/fs/vfs.h`, add:

```c
#define VFS_MOUNT_SOURCE_MAX 16
```

Declare:

```c
typedef struct {
    char source[VFS_MOUNT_SOURCE_MAX];
    char path[128];
    char fstype[VFS_FS_NAME_MAX];
    char options[48];
} vfs_mount_info_t;

uint32_t vfs_mount_count(void);
int vfs_mount_info_at(uint32_t index, vfs_mount_info_t *out);
int vfs_mount_with_source(const char *mount_path, const char *fs_name,
                          const char *source);
```

In `kernel/fs/vfs.c`, add `source`, `fstype`, and `options` to `vfs_mount_t`. Implement `vfs_mount_with_source()` as the worker and make `vfs_mount()` call it with source defaults:

```c
int vfs_mount(const char *mount_path, const char *fs_name)
{
    return vfs_mount_with_source(mount_path, fs_name, fs_name);
}
```

When mounting, set:

```c
k_strncpy(vfs_mounts[i].source, source ? source : fs_name, sizeof(vfs_mounts[i].source) - 1);
k_strncpy(vfs_mounts[i].fstype, fs_name, sizeof(vfs_mounts[i].fstype) - 1);
```

Set options by kind:

```c
"rw"
"rw,nosuid"
"rw,nosuid,nodev,noexec,relatime"
```

for real filesystems, devfs, and proc/sysfs respectively.

- [ ] **Step 4: Render `/proc/mounts` from VFS**

In `kernel/fs/procfs.c`, replace `procfs_render_mounts` body:

```c
static void procfs_render_mounts(render_buf_t *rb)
{
    vfs_mount_info_t info;

    for (uint32_t i = 0; i < vfs_mount_count(); i++) {
        if (vfs_mount_info_at(i, &info) != 0)
            continue;
        procfs_emitf(rb, "%s %s %s %s 0 0\n",
                     info.source,
                     info.path[0] ? info.path : "/",
                     info.fstype,
                     info.options);
    }
}
```

- [ ] **Step 5: Verify tests**

Run:

```bash
make KTEST=1 kernel
make check
```

Expected: dynamic mounts test passes.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/vfs.h kernel/fs/vfs.c kernel/fs/procfs.h kernel/fs/procfs.c kernel/test/test_vfs.c
git commit -m "feat: render dynamic proc mounts"
```

---

### Task 9: Mount `/sys` And Use Device Sources At Boot

**Files:**
- Modify: `kernel/kernel.c`
- Modify: `kernel/fs/fs.c`
- Modify: `kernel/fs/ext3.c`
- Modify: `docs/ch14-vfs.md`
- Modify: `README.md`

- [ ] **Step 1: Mount root and DUFS with block-device sources**

In `kernel/kernel.c`, change root mounting:

```c
if (vfs_mount_with_source("/", DRUNIX_ROOT_FS, "/dev/sda1") != 0)
```

Change DUFS secondary mount:

```c
if (k_strcmp(DRUNIX_ROOT_FS, "ext3") == 0) {
    if (vfs_mount_with_source("/dufs", "dufs", "/dev/sdb1") != 0)
        klog("FS", "dufs mount at /dufs failed");
    else
        klog("FS", "dufs mounted at /dufs");
}
```

- [ ] **Step 2: Mount sysfs**

After procfs mounts in `kernel/kernel.c`, add:

```c
if (vfs_mount_with_source("/sys", "sysfs", "sysfs") != 0)
{
    klog("FS", "sysfs mount failed");
    for (;;)
        __asm__ volatile("hlt");
}
klog("FS", "sysfs mounted at /sys");
```

- [ ] **Step 3: Update docs**

In `README.md`, replace raw image language with:

```markdown
`disk.img` is an MBR disk image. Its first primary partition appears as
`/dev/sda1` inside Drunix and holds the configured root filesystem. The default
root is ext3. `dufs.img` is the secondary MBR disk image; its first primary
partition appears as `/dev/sdb1` and is mounted at `/dufs` during ext3-root
boots.
```

In `docs/ch14-vfs.md`, update the mount overview to mention `/sys` and block
device sources.

- [ ] **Step 4: Verify boot tests**

Run:

```bash
make clean
make check
```

Expected: partitioned images build, kernel tests boot, and KTEST summary has `fail=0`.

- [ ] **Step 5: Commit**

```bash
git add kernel/kernel.c kernel/fs/fs.c kernel/fs/ext3.c README.md docs/ch14-vfs.md
git commit -m "feat: mount sysfs and sd partition sources"
```

---

### Task 10: Add Userland `lsblk`

**Files:**
- Create: `user/lsblk.cpp`
- Modify: `user/Makefile`
- Modify: `Makefile`

- [ ] **Step 1: Add a failing `lsblk` build target**

Add `lsblk` to root `Makefile` `USER_PROGS`.

Add `lsblk` to `PROGS` and `CXX_PROGS` in `user/Makefile`.

Run:

```bash
make user/lsblk
```

Expected: build fails because `user/lsblk.cpp` does not exist.

- [ ] **Step 2: Implement small file and directory helpers**

Create `user/lsblk.cpp` with these helpers first:

```cpp
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * lsblk.cpp - list Drunix block devices using /sys/block.
 */

#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "lib/syscall.h"

static int read_file(const char *path, char *buf, int cap)
{
    FILE *f;
    int n;

    if (cap <= 0)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = (int)fread(buf, 1, (size_t)(cap - 1), f);
    fclose(f);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return n;
}

static int list_dir(const char *path, char *buf, int cap)
{
    return sys_getdents(path, buf, cap);
}

static int streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}
```

- [ ] **Step 3: Add device model parsing**

Add:

```cpp
#define MAX_DEVS 16

struct dev_t {
    char name[16];
    char parent[16];
    char type[8];
    char majmin[16];
    char mount[64];
    char fstype[16];
    char uuid[40];
    unsigned int size_sectors;
    unsigned int start;
    unsigned int partition;
};

static dev_t devs[MAX_DEVS];
static int dev_count;

static unsigned int parse_uint(const char *s)
{
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s - '0');
        s++;
    }
    return v;
}
```

Add a function that reads `/sys/block/<disk>` and one child level:

```cpp
static void add_dev(const char *name, const char *parent)
{
    dev_t *d;
    char path[96];
    char text[64];

    if (dev_count >= MAX_DEVS)
        return;
    d = &devs[dev_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    strncpy(d->parent, parent ? parent : "", sizeof(d->parent) - 1);

    snprintf(path, sizeof(path), parent && parent[0]
             ? "/sys/block/%s/%s/size" : "/sys/block/%s/size",
             parent && parent[0] ? parent : name,
             name);
    if (read_file(path, text, sizeof(text)) > 0)
        d->size_sectors = parse_uint(text);

    snprintf(path, sizeof(path), parent && parent[0]
             ? "/sys/block/%s/%s/dev" : "/sys/block/%s/dev",
             parent && parent[0] ? parent : name,
             name);
    read_file(path, d->majmin, sizeof(d->majmin));

    snprintf(path, sizeof(path), parent && parent[0]
             ? "/sys/block/%s/%s/type" : "/sys/block/%s/type",
             parent && parent[0] ? parent : name,
             name);
    read_file(path, d->type, sizeof(d->type));

    if (parent && parent[0]) {
        snprintf(path, sizeof(path), "/sys/block/%s/%s/start", parent, name);
        if (read_file(path, text, sizeof(text)) > 0)
            d->start = parse_uint(text);
        snprintf(path, sizeof(path), "/sys/block/%s/%s/partition", parent, name);
        if (read_file(path, text, sizeof(text)) > 0)
            d->partition = parse_uint(text);
    }
}
```

- [ ] **Step 4: Add `/sys/block` traversal**

Add:

```cpp
static int load_sys_block(void)
{
    char dents[512];
    int n = list_dir("/sys/block", dents, sizeof(dents));

    if (n < 0)
        return -1;
    for (int i = 0; i < n; ) {
        char *name = dents + i;
        int len = (int)strlen(name);
        if (len > 0 && name[len - 1] == '/')
            name[len - 1] = '\0';
        if (name[0]) {
            char child_path[64];
            char child_dents[512];
            int cn;

            add_dev(name, "");
            snprintf(child_path, sizeof(child_path), "/sys/block/%s", name);
            cn = list_dir(child_path, child_dents, sizeof(child_dents));
            if (cn > 0) {
                for (int j = 0; j < cn; ) {
                    char *child = child_dents + j;
                    int clen = (int)strlen(child);
                    if (clen > 0 && child[clen - 1] == '/') {
                        child[clen - 1] = '\0';
                        add_dev(child, name);
                    }
                    j += clen + 1;
                }
            }
        }
        i += len + 1;
    }
    return 0;
}
```

- [ ] **Step 5: Add mountpoint association**

Add:

```cpp
static void load_mounts(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[160];

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char src[32], mnt[64], type[16];
        if (split3(line, src, sizeof(src), mnt, sizeof(mnt), type, sizeof(type)) != 0)
            continue;
        if (strncmp(src, "/dev/", 5) != 0)
            continue;
        for (int i = 0; i < dev_count; i++) {
            if (streq(devs[i].name, src + 5)) {
                strncpy(devs[i].mount, mnt, sizeof(devs[i].mount) - 1);
                strncpy(devs[i].fstype, type, sizeof(devs[i].fstype) - 1);
            }
        }
    }
    fclose(f);
}
```

Add this local parser before `load_mounts()`:

```cpp
static int copy_field(const char **p, char *out, int cap)
{
    int n = 0;

    while (**p == ' ' || **p == '\t')
        (*p)++;
    if (**p == '\0' || **p == '\n')
        return -1;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n') {
        if (n + 1 < cap)
            out[n++] = **p;
        (*p)++;
    }
    out[n] = '\0';
    return 0;
}

static int split3(const char *line, char *a, int asz, char *b, int bsz,
                  char *c, int csz)
{
    const char *p = line;

    if (copy_field(&p, a, asz) != 0)
        return -1;
    if (copy_field(&p, b, bsz) != 0)
        return -1;
    if (copy_field(&p, c, csz) != 0)
        return -1;
    return 0;
}
```

- [ ] **Step 6: Add filesystem probing for `-f`**

Add superblock probing through `/dev/<name>`. Read the first 2048 bytes from
each partition device. ext-family superblocks live 1024 bytes from the start of
the filesystem, and DUFS stores its superblock in LBA 1.

```cpp
static unsigned int le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static int read_dev_prefix(const char *name, unsigned char *buf, int cap)
{
    char path[32];
    FILE *f;
    int n;

    snprintf(path, sizeof(path), "/dev/%s", name);
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

static void format_uuid(const unsigned char *u, char *out, int cap)
{
    snprintf(out, (size_t)cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void probe_filesystems(void)
{
    unsigned char buf[2048];

    for (int i = 0; i < dev_count; i++) {
        if (!streq(devs[i].type, "part"))
            continue;
        if (read_dev_prefix(devs[i].name, buf, sizeof(buf)) < (int)sizeof(buf))
            continue;
        if (le16(buf + 1024 + 56) == 0xEF53u) {
            strncpy(devs[i].fstype, "ext3", sizeof(devs[i].fstype) - 1);
            format_uuid(buf + 1024 + 104, devs[i].uuid, sizeof(devs[i].uuid));
            continue;
        }
        if (le32(buf + 512) == 0x44554603u) {
            strncpy(devs[i].fstype, "dufs", sizeof(devs[i].fstype) - 1);
            continue;
        }
    }
}
```

Keep mountpoint association separate: probing fills `FSTYPE` and `UUID`, while
`/proc/mounts` fills `MOUNTPOINTS`.

- [ ] **Step 7: Add formatters and main**

Add:

```cpp
static unsigned int size_mb(unsigned int sectors)
{
    return (sectors + 2047u) / 2048u;
}

static void print_default(void)
{
    printf("NAME    MAJ:MIN RM  SIZE RO TYPE MOUNTPOINTS\n");
    for (int i = 0; i < dev_count; i++) {
        if (devs[i].parent[0])
            continue;
        printf("%-7s %-7s  0 %4uM  0 %-4s %s\n",
               devs[i].name, devs[i].majmin, size_mb(devs[i].size_sectors),
               devs[i].type, devs[i].mount);
        for (int j = 0; j < dev_count; j++) {
            if (!streq(devs[j].parent, devs[i].name))
                continue;
            printf("`-%-5s %-7s  0 %4uM  0 %-4s %s\n",
                   devs[j].name, devs[j].majmin, size_mb(devs[j].size_sectors),
                   devs[j].type, devs[j].mount);
        }
    }
}

static void print_fs(void)
{
    printf("NAME    FSTYPE LABEL UUID MOUNTPOINTS\n");
    for (int i = 0; i < dev_count; i++) {
        if (devs[i].parent[0])
            continue;
        printf("%-7s %-6s %-5s %-4s %s\n",
               devs[i].name, devs[i].fstype, "", devs[i].uuid, devs[i].mount);
        for (int j = 0; j < dev_count; j++) {
            if (!streq(devs[j].parent, devs[i].name))
                continue;
            printf("`-%-5s %-6s %-5s %-4s %s\n",
                   devs[j].name, devs[j].fstype, "", devs[j].uuid, devs[j].mount);
        }
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: lsblk [-f]\n");
}

int main(int argc, char **argv)
{
    int fs_mode = 0;

    if (argc == 2 && streq(argv[1], "-f"))
        fs_mode = 1;
    else if (argc == 2 && streq(argv[1], "--help")) {
        usage();
        return 0;
    } else if (argc != 1) {
        usage();
        return 1;
    }

    if (load_sys_block() != 0) {
        fprintf(stderr, "lsblk: cannot read /sys/block\n");
        return 1;
    }
    load_mounts();
    probe_filesystems();
    if (fs_mode)
        print_fs();
    else
        print_default();
    return 0;
}
```

- [ ] **Step 8: Build `lsblk`**

Run:

```bash
make user/lsblk
```

Expected: build succeeds.

- [ ] **Step 9: Boot `lsblk` as init for smoke output**

Run:

```bash
make INIT_PROGRAM=bin/lsblk INIT_ARG0=lsblk run-stdio
```

Expected: QEMU boots and debug/serial output shows `sda`, `sda1`, `sdb`, and `sdb1`. Stop QEMU manually if it remains running.

- [ ] **Step 10: Boot `lsblk -f` through the shell**

Run:

```bash
make run-stdio
```

In the shell, run:

```sh
lsblk -f
```

Expected: output includes `ext3` on `sda1`, `dufs` on `sdb1`, `/`, and `/dufs`.

- [ ] **Step 11: Commit**

```bash
git add user/lsblk.cpp user/Makefile Makefile
git commit -m "feat: add lsblk utility"
```

---

### Task 11: Final Integration And Regression Checks

**Files:**
- Modify only if checks expose a concrete issue.

- [ ] **Step 1: Run full headless kernel tests**

Run:

```bash
make clean
make check
```

Expected: KTEST summary reports `fail=0`.

- [ ] **Step 2: Run ext3 Linux compatibility**

Run:

```bash
make test-ext3-linux-compat
```

Expected: generated partitioned image passes Drunix ext3 write smoke and host ext3 validation.

- [ ] **Step 3: Run BusyBox compatibility if storage changes affected Linux ABI**

Run:

```bash
make test-busybox-compat
```

Expected: existing BusyBox compatibility report still passes or has only known unrelated failures.

- [ ] **Step 4: Inspect final `lsblk` output**

Run:

```bash
make run-stdio
```

In Drunix shell:

```sh
lsblk
lsblk -f
cat /proc/mounts
```

Expected:

```text
sda
`-sda1
sdb
`-sdb1
```

Expected `/proc/mounts` includes:

```text
/dev/sda1 / ext3 rw 0 0
/dev/sdb1 /dufs dufs rw 0 0
sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0
```

- [ ] **Step 5: Commit any verification fixes**

If changes were needed, inspect the exact files and commit them:

```bash
git status --short
git add kernel/drivers/blkdev.c kernel/drivers/blkdev_part.c kernel/fs/vfs.c kernel/fs/sysfs.c kernel/fs/procfs.c kernel/proc/syscall.c user/lsblk.cpp Makefile
git commit -m "fix: stabilize linux-style block listing"
```

If no changes were needed, do not create an empty commit. If the verification
fix touched a different tracked file, add that exact path in the same command
before committing.

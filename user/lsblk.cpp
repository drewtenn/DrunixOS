/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * lsblk.cpp - list Drunix block devices using /sys/block.
 */

#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "lib/syscall.h"

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

static int streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static void copy_string(char *dst, int cap, const char *src)
{
    if (!dst || cap <= 0)
        return;
    if (!src)
        src = "";
    strncpy(dst, src, (size_t)cap - 1u);
    dst[cap - 1] = '\0';
}

static unsigned int parse_uint(const char *s)
{
    unsigned int v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s - '0');
        s++;
    }
    return v;
}

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
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return n;
}

static int list_dir(const char *path, char *buf, int cap)
{
    return sys_getdents(path, buf, cap);
}

static void sysfs_path(char *path, int cap, const char *name,
                       const char *parent, const char *leaf)
{
    if (parent && parent[0])
        snprintf(path, (size_t)cap, "/sys/block/%s/%s/%s",
                 parent, name, leaf);
    else
        snprintf(path, (size_t)cap, "/sys/block/%s/%s", name, leaf);
}

static void add_dev(const char *name, const char *parent)
{
    dev_t *d;
    char path[96];
    char text[64];

    if (dev_count >= MAX_DEVS)
        return;
    d = &devs[dev_count++];
    memset(d, 0, sizeof(*d));
    copy_string(d->name, (int)sizeof(d->name), name);
    copy_string(d->parent, (int)sizeof(d->parent), parent ? parent : "");

    sysfs_path(path, (int)sizeof(path), name, parent, "size");
    if (read_file(path, text, (int)sizeof(text)) > 0)
        d->size_sectors = parse_uint(text);

    sysfs_path(path, (int)sizeof(path), name, parent, "dev");
    read_file(path, d->majmin, (int)sizeof(d->majmin));

    sysfs_path(path, (int)sizeof(path), name, parent, "type");
    read_file(path, d->type, (int)sizeof(d->type));

    if (parent && parent[0]) {
        sysfs_path(path, (int)sizeof(path), name, parent, "start");
        if (read_file(path, text, (int)sizeof(text)) > 0)
            d->start = parse_uint(text);
        sysfs_path(path, (int)sizeof(path), name, parent, "partition");
        if (read_file(path, text, (int)sizeof(text)) > 0)
            d->partition = parse_uint(text);
    }
}

static int load_sys_block(void)
{
    char dents[512];
    int n = list_dir("/sys/block", dents, (int)sizeof(dents));

    if (n < 0)
        return -1;
    for (int i = 0; i < n; ) {
        char *name = dents + i;
        int len = (int)strlen(name);
        int advance = len + 1;

        if (len > 0 && name[len - 1] == '/')
            name[len - 1] = '\0';
        if (name[0]) {
            char child_path[64];
            char child_dents[512];
            int cn;

            add_dev(name, "");
            snprintf(child_path, sizeof(child_path), "/sys/block/%s", name);
            cn = list_dir(child_path, child_dents, (int)sizeof(child_dents));
            if (cn > 0) {
                for (int j = 0; j < cn; ) {
                    char *child = child_dents + j;
                    int clen = (int)strlen(child);
                    int cadvance = clen + 1;

                    if (clen > 0 && child[clen - 1] == '/') {
                        child[clen - 1] = '\0';
                        add_dev(child, name);
                    }
                    j += cadvance;
                }
            }
        }
        i += advance;
    }
    return 0;
}

static int copy_field(const char **p, char *out, int cap)
{
    int n = 0;

    while (**p == ' ' || **p == '\t')
        (*p)++;
    if (**p == '\0' || **p == '\n')
        return -1;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n') {
        char ch = **p;

        if (ch == '\\' &&
            (*p)[1] >= '0' && (*p)[1] <= '7' &&
            (*p)[2] >= '0' && (*p)[2] <= '7' &&
            (*p)[3] >= '0' && (*p)[3] <= '7') {
            ch = (char)(((*p)[1] - '0') * 64 +
                        ((*p)[2] - '0') * 8 +
                        ((*p)[3] - '0'));
            *p += 4;
        } else {
            (*p)++;
        }

        if (n + 1 < cap)
            out[n++] = ch;
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

static void load_mounts(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[256];

    if (!f)
        return;
    while (fgets(line, (int)sizeof(line), f)) {
        char src[64];
        char mnt[96];
        char type[32];

        if (split3(line, src, (int)sizeof(src), mnt, (int)sizeof(mnt),
                   type, (int)sizeof(type)) != 0)
            continue;
        if (strncmp(src, "/dev/", 5) != 0)
            continue;
        for (int i = 0; i < dev_count; i++) {
            if (streq(devs[i].name, src + 5)) {
                copy_string(devs[i].mount, (int)sizeof(devs[i].mount), mnt);
                copy_string(devs[i].fstype, (int)sizeof(devs[i].fstype), type);
            }
        }
    }
    fclose(f);
}

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
        if (read_dev_prefix(devs[i].name, buf, (int)sizeof(buf)) <
            (int)sizeof(buf))
            continue;
        if (le16(buf + 1024 + 56) == 0xEF53u) {
            copy_string(devs[i].fstype, (int)sizeof(devs[i].fstype), "ext3");
            format_uuid(buf + 1024 + 104, devs[i].uuid,
                        (int)sizeof(devs[i].uuid));
            continue;
        }
        if (le32(buf + 512) == 0x44554603u) {
            copy_string(devs[i].fstype, (int)sizeof(devs[i].fstype), "dufs");
            continue;
        }
    }
}

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
                   devs[j].name, devs[j].majmin,
                   size_mb(devs[j].size_sectors), devs[j].type,
                   devs[j].mount);
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
               devs[i].name, devs[i].fstype, "", devs[i].uuid,
               devs[i].mount);
        for (int j = 0; j < dev_count; j++) {
            if (!streq(devs[j].parent, devs[i].name))
                continue;
            printf("`-%-5s %-6s %-5s %-4s %s\n",
                   devs[j].name, devs[j].fstype, "", devs[j].uuid,
                   devs[j].mount);
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

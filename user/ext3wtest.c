/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ext3wtest.c - smoke test for writable ext3 root support.
 */

#include "lib/string.h"
#include "lib/syscall.h"

static int write_all(int fd, const char *buf, int len)
{
    int off = 0;

    while (off < len)
    {
        int n = sys_fwrite(fd, buf + off, len - off);
        if (n <= 0)
            return -1;
        off += n;
    }
    return 0;
}

static int read_file(const char *path, char *buf, int cap)
{
    int fd = sys_open(path);
    int total = 0;

    if (fd < 0)
        return -1;
    while (total < cap - 1)
    {
        int n = sys_read(fd, buf + total, cap - 1 - total);
        if (n < 0)
        {
            sys_close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    buf[total] = '\0';
    sys_close(fd);
    return total;
}

static void log_line(int fd, const char *msg)
{
    write_all(fd, msg, (int)strlen(msg));
    write_all(fd, "\n", 1);
}

static int write_file(const char *path, const char *text)
{
    int fd = sys_create(path);
    int len = (int)strlen(text);

    if (fd < 0)
        return -1;
    if (write_all(fd, text, len) != 0)
    {
        sys_close(fd);
        return -1;
    }
    sys_close(fd);
    return 0;
}

int main(void)
{
    char buf[128];
    int logfd;
    int ok = 1;

    sys_unlink("/dufs/ext3wtest.log");
    logfd = sys_create("/dufs/ext3wtest.log");
    if (logfd < 0)
        return 1;

    sys_unlink("/ext3w.txt");
    sys_unlink("/ext3wdir/nested.txt");
    sys_rmdir("/ext3wdir");

    if (write_file("/ext3w.txt", "alpha\nbeta\n") != 0 ||
        read_file("/ext3w.txt", buf, sizeof(buf)) < 0 ||
        strcmp(buf, "alpha\nbeta\n") != 0)
    {
        log_line(logfd, "FAIL root file write");
        ok = 0;
    }

    if (write_file("/ext3w.txt", "short\n") != 0 ||
        read_file("/ext3w.txt", buf, sizeof(buf)) < 0 ||
        strcmp(buf, "short\n") != 0)
    {
        log_line(logfd, "FAIL existing file truncate");
        ok = 0;
    }

    if (sys_mkdir("/ext3wdir") != 0 ||
        write_file("/ext3wdir/nested.txt", "nested\n") != 0 ||
        read_file("/ext3wdir/nested.txt", buf, sizeof(buf)) < 0 ||
        strcmp(buf, "nested\n") != 0)
    {
        log_line(logfd, "FAIL nested file write");
        ok = 0;
    }

    if (sys_unlink("/ext3w.txt") != 0 || sys_open("/ext3w.txt") >= 0)
    {
        log_line(logfd, "FAIL unlink");
        ok = 0;
    }

    log_line(logfd, ok ? "EXT3WTEST PASS" : "EXT3WTEST FAIL");
    sys_close(logfd);
    return ok ? 0 : 1;
}

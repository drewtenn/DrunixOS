/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * which.c — user-space PATH lookup utility.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

static int has_slash(const char *s)
{
    for (; *s; s++)
        if (*s == '/')
            return 1;
    return 0;
}

static int is_file(const char *path)
{
    int fd = sys_open(path);
    if (fd < 0)
        return 0;
    sys_close(fd);
    return 1;
}

static int build_path(const char *dir, int dir_len,
                      const char *name, char *out, int outsz)
{
    int i = 0;

    if (outsz <= 0)
        return -1;

    if (dir_len == 0)
    {
        while (i < outsz - 1 && name[i])
        {
            out[i] = name[i];
            i++;
        }
        out[i] = '\0';
        return name[i] ? -1 : 0;
    }

    for (int j = 0; j < dir_len && i < outsz - 1; j++)
        out[i++] = dir[j];
    if (i > 0 && out[i - 1] != '/' && i < outsz - 1)
        out[i++] = '/';
    for (int j = 0; name[j] && i < outsz - 1; j++)
        out[i++] = name[j];
    out[i] = '\0';
    return 0;
}

static int which_one(const char *name, int all)
{
    const char *path;
    const char *start;
    int found = 0;

    if (has_slash(name))
    {
        if (is_file(name))
        {
            printf("%s\n", name);
            return 0;
        }
        return 1;
    }

    path = getenv("PATH");
    if (!path || !*path)
        return 1;

    start = path;
    for (;;)
    {
        const char *end = start;
        char candidate[128];
        int len;

        while (*end && *end != ':')
            end++;
        len = (int)(end - start);

        if (build_path(start, len, name, candidate, sizeof(candidate)) == 0 &&
            is_file(candidate))
        {
            printf("%s\n", candidate);
            found = 1;
            if (!all)
                return 0;
        }

        if (!*end)
            break;
        start = end + 1;
    }

    return found ? 0 : 1;
}

int main(int argc, char **argv)
{
    int rc = 0;
    int all = 0;
    int first = 1;

    if (argc < 2)
    {
        printf("usage: which [-a] command...\n");
        return 2;
    }

    if (!strcmp(argv[1], "-a"))
    {
        all = 1;
        first = 2;
    }

    if (first >= argc)
    {
        printf("usage: which [-a] command...\n");
        return 2;
    }

    for (int i = first; i < argc; i++)
        if (which_one(argv[i], all) != 0)
            rc = 1;

    return rc;
}

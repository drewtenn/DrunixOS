/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * basename.c — user-space basename utility.
 */

#include "stdio.h"
#include "string.h"

static const char *base_component(char *path)
{
    size_t len;

    if (!path || !*path)
        return ".";

    len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    if (len == 1 && path[0] == '/')
        return "/";

    char *slash = strrchr(path, '/');
    if (!slash)
        return path;
    return slash + 1;
}

int main(int argc, char **argv)
{
    char path[512];
    const char *base;

    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: basename string [suffix]\n");
        return 2;
    }

    strncpy(path, argv[1], sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    base = base_component(path);

    if (argc == 3 && strcmp(base, "/") != 0)
    {
        size_t blen = strlen(base);
        size_t slen = strlen(argv[2]);
        if (slen > 0 && slen < blen &&
            strcmp(base + blen - slen, argv[2]) == 0)
            ((char *)base)[blen - slen] = '\0';
    }

    printf("%s\n", base);
    return 0;
}

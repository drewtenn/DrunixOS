/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * dirname.c — user-space dirname utility.
 */

#include "stdio.h"
#include "string.h"

int main(int argc, char **argv)
{
    char path[512];
    size_t len;
    char *slash;

    if (argc != 2)
    {
        fprintf(stderr, "usage: dirname string\n");
        return 2;
    }

    strncpy(path, argv[1], sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    if (path[0] == '\0')
    {
        puts(".");
        return 0;
    }

    len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    slash = strrchr(path, '/');
    if (!slash)
    {
        puts(".");
        return 0;
    }

    if (slash == path)
    {
        path[1] = '\0';
    }
    else
    {
        while (slash > path && *slash == '/')
            *slash-- = '\0';
    }

    if (path[0] == '\0')
        puts("/");
    else
        puts(path);

    return 0;
}

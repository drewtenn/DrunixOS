/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * tee.c — user-space tee utility.
 */

#include "lib/stdio.h"
#include "lib/string.h"

#define MAX_TEE_FILES 8

int main(int argc, char **argv)
{
    FILE *files[MAX_TEE_FILES];
    int file_count = 0;
    int first = 1;
    int rc = 0;
    char buf[256];

    if (argc > 1 && !strcmp(argv[1], "-a"))
    {
        fprintf(stderr, "tee: append mode is not supported yet\n");
        return 2;
    }

    if (argc > 1 && !strcmp(argv[1], "--"))
        first = 2;

    for (int i = first; i < argc && file_count < MAX_TEE_FILES; i++)
    {
        FILE *f = fopen(argv[i], "w");
        if (!f)
        {
            fprintf(stderr, "tee: cannot open: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        files[file_count++] = f;
    }

    if (argc - first > MAX_TEE_FILES)
    {
        fprintf(stderr, "tee: too many files\n");
        rc = 1;
    }

    for (;;)
    {
        size_t n = fread(buf, 1, sizeof(buf), stdin);
        if (n == 0)
            break;

        fwrite(buf, 1, n, stdout);
        for (int i = 0; i < file_count; i++)
            fwrite(buf, 1, n, files[i]);
    }

    for (int i = 0; i < file_count; i++)
        fclose(files[i]);

    return rc;
}

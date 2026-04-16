/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * cat.c — user-space cat utility.
 */

#include "lib/stdio.h"
#include "lib/string.h"

static int cat_stream(FILE *f)
{
    char buf[512];

    for (;;)
    {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
            break;
        if (fwrite(buf, 1, n, stdout) != n)
            return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc == 1)
        return cat_stream(stdin) == 0 ? 0 : 1;

    for (int i = 1; i < argc; i++)
    {
        FILE *f;

        if (!strcmp(argv[i], "-"))
            f = stdin;
        else
            f = fopen(argv[i], "r");

        if (!f)
        {
            fprintf(stderr, "cat: cannot open: %s\n", argv[i]);
            rc = 1;
            continue;
        }

        if (cat_stream(f) != 0)
            rc = 1;
        if (f != stdin)
            fclose(f);
    }

    return rc;
}

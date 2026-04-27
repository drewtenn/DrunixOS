/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * cmp.c — user-space cmp utility.
 */

#include "lib/stdio.h"
#include "lib/string.h"

static int usage(void)
{
    fprintf(stderr, "usage: cmp [-l|-s] file1 file2\n");
    return 2;
}

int main(int argc, char **argv)
{
    int list = 0;
    int silent = 0;
    int first = 1;
    FILE *a;
    FILE *b;
    unsigned int byte_no = 1;
    unsigned int line_no = 1;
    int different = 0;

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0')
    {
        if (!strcmp(argv[1], "-l"))
            list = 1;
        else if (!strcmp(argv[1], "-s"))
            silent = 1;
        else
            return usage();
        first = 2;
    }

    if (argc - first != 2)
        return usage();

    a = !strcmp(argv[first], "-") ? stdin : fopen(argv[first], "r");
    if (!a)
    {
        if (!silent)
            fprintf(stderr, "cmp: cannot open: %s\n", argv[first]);
        return 2;
    }

    b = !strcmp(argv[first + 1], "-") ? stdin : fopen(argv[first + 1], "r");
    if (!b)
    {
        if (!silent)
            fprintf(stderr, "cmp: cannot open: %s\n", argv[first + 1]);
        if (a != stdin)
            fclose(a);
        return 2;
    }

    for (;;)
    {
        int ca = fgetc(a);
        int cb = fgetc(b);

        if (ca == EOF && cb == EOF)
            break;

        if (ca == EOF || cb == EOF)
        {
            different = 1;
            if (!silent)
            {
                const char *name = ca == EOF ? argv[first] : argv[first + 1];
                fprintf(stderr, "cmp: EOF on %s after byte %u\n",
                        name, byte_no - 1);
            }
            break;
        }

        if (ca != cb)
        {
            different = 1;
            if (silent)
                break;
            if (list)
                printf("%u %o %o\n", byte_no, ca & 0377, cb & 0377);
            else
            {
                printf("%s %s differ: char %u, line %u\n",
                       argv[first], argv[first + 1], byte_no, line_no);
                break;
            }
        }

        if (ca == '\n')
            line_no++;
        byte_no++;
    }

    if (a != stdin)
        fclose(a);
    if (b != stdin)
        fclose(b);

    return different ? 1 : 0;
}

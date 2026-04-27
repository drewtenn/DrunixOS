/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * head.c — user-space head utility.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"

static int parse_count(const char *s, int *out)
{
    int n = 0;

    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; p++)
    {
        if (*p < '0' || *p > '9')
            return 0;
        n = n * 10 + (*p - '0');
    }
    *out = n;
    return 1;
}

static void print_head(FILE *f, int max_lines)
{
    int lines = 0;

    while (lines < max_lines)
    {
        int c = fgetc(f);
        if (c == EOF)
            break;
        putchar(c);
        if (c == '\n')
            lines++;
    }
}

int main(int argc, char **argv)
{
    int max_lines = 10;
    int first = 1;
    int rc = 0;
    int multiple;

    if (argc > 1 && !strcmp(argv[1], "-n"))
    {
        if (argc < 3 || !parse_count(argv[2], &max_lines))
        {
            fprintf(stderr, "usage: head [-n lines] [file...]\n");
            return 2;
        }
        first = 3;
    }
    else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' &&
             argv[1][1] <= '9')
    {
        if (!parse_count(argv[1] + 1, &max_lines))
        {
            fprintf(stderr, "usage: head [-n lines] [file...]\n");
            return 2;
        }
        first = 2;
    }

    if (first == argc)
    {
        print_head(stdin, max_lines);
        return 0;
    }

    multiple = argc - first > 1;
    for (int i = first; i < argc; i++)
    {
        FILE *f;
        if (!strcmp(argv[i], "-"))
            f = stdin;
        else
            f = fopen(argv[i], "r");
        if (!f)
        {
            fprintf(stderr, "head: cannot open: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        if (multiple)
            printf("%s==> %s <==\n", i == first ? "" : "\n", argv[i]);
        print_head(f, max_lines);
        if (f != stdin)
            fclose(f);
    }

    return rc;
}

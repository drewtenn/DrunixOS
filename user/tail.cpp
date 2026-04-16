/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * tail.c — user-space tail utility.
 */

#include "lib/stdio.h"
#include "lib/stdlib.h"
#include "lib/string.h"

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

static int print_tail(FILE *f, int max_lines)
{
    char **lines;
    size_t *lengths;
    int start = 0;
    int count = 0;
    char *line = 0;
    size_t cap = 0;
    ssize_t len;

    if (max_lines <= 0)
        return 0;

    lines = (char **)malloc(sizeof(char *) * (size_t)max_lines);
    lengths = (size_t *)malloc(sizeof(size_t) * (size_t)max_lines);
    if (!lines || !lengths)
    {
        free(lines);
        free(lengths);
        return -1;
    }

    for (int i = 0; i < max_lines; i++)
    {
        lines[i] = 0;
        lengths[i] = 0;
    }

    while ((len = getline(&line, &cap, f)) >= 0)
    {
        int slot;
        if (count < max_lines)
        {
            slot = count++;
        }
        else
        {
            slot = start;
            free(lines[slot]);
            start = (start + 1) % max_lines;
        }

        lines[slot] = line;
        lengths[slot] = (size_t)len;
        line = 0;
        cap = 0;
    }
    free(line);

    for (int i = 0; i < count; i++)
    {
        int slot = (start + i) % max_lines;
        fwrite(lines[slot], 1, lengths[slot], stdout);
        free(lines[slot]);
    }

    free(lines);
    free(lengths);
    return 0;
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
            fprintf(stderr, "usage: tail [-n lines] [file...]\n");
            return 2;
        }
        first = 3;
    }
    else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' &&
             argv[1][1] <= '9')
    {
        if (!parse_count(argv[1] + 1, &max_lines))
        {
            fprintf(stderr, "usage: tail [-n lines] [file...]\n");
            return 2;
        }
        first = 2;
    }

    if (first == argc)
        return print_tail(stdin, max_lines) == 0 ? 0 : 1;

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
            fprintf(stderr, "tail: cannot open: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        if (multiple)
            printf("%s==> %s <==\n", i == first ? "" : "\n", argv[i]);
        if (print_tail(f, max_lines) != 0)
        {
            fprintf(stderr, "tail: out of memory\n");
            rc = 1;
        }
        if (f != stdin)
            fclose(f);
    }

    return rc;
}

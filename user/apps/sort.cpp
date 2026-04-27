/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * sort.c — user-space sort utility.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"

typedef struct
{
    char *text;
} line_t;

static int reverse_order = 0;

static int line_cmp(const void *a, const void *b)
{
    const line_t *la = (const line_t *)a;
    const line_t *lb = (const line_t *)b;
    int r = strcmp(la->text, lb->text);
    return reverse_order ? -r : r;
}

static int add_line(line_t **lines, int *count, int *cap, char *line)
{
    if (*count >= *cap)
    {
        int new_cap = *cap ? *cap * 2 : 64;
        line_t *new_lines = (line_t *)realloc(*lines, sizeof(line_t) * (size_t)new_cap);
        if (!new_lines)
            return -1;
        *lines = new_lines;
        *cap = new_cap;
    }
    (*lines)[*count].text = line;
    (*count)++;
    return 0;
}

static int read_stream(FILE *f, line_t **lines, int *count, int *cap)
{
    char *line = 0;
    size_t n = 0;
    ssize_t len;

    while ((len = getline(&line, &n, f)) >= 0)
    {
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (add_line(lines, count, cap, line) != 0)
        {
            free(line);
            return -1;
        }
        line = 0;
        n = 0;
    }
    free(line);
    return 0;
}

int main(int argc, char **argv)
{
    int unique = 0;
    int first = 1;
    int rc = 0;
    line_t *lines = 0;
    int count = 0;
    int cap = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1])
    {
        if (!strcmp(argv[first], "--"))
        {
            first++;
            break;
        }
        for (int i = 1; argv[first][i]; i++)
        {
            if (argv[first][i] == 'r')
                reverse_order = 1;
            else if (argv[first][i] == 'u')
                unique = 1;
            else
            {
                fprintf(stderr, "sort: invalid option: %s\n", argv[first]);
                return 2;
            }
        }
        first++;
    }

    if (first == argc)
    {
        if (read_stream(stdin, &lines, &count, &cap) != 0)
        {
            fprintf(stderr, "sort: out of memory\n");
            return 2;
        }
    }
    else
    {
        for (int i = first; i < argc; i++)
        {
            FILE *f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
            if (!f)
            {
                fprintf(stderr, "sort: cannot open: %s\n", argv[i]);
                rc = 2;
                continue;
            }
            if (read_stream(f, &lines, &count, &cap) != 0)
            {
                fprintf(stderr, "sort: out of memory\n");
                rc = 2;
            }
            if (f != stdin)
                fclose(f);
        }
    }

    qsort(lines, (size_t)count, sizeof(line_t), line_cmp);

    for (int i = 0; i < count; i++)
    {
        if (unique && i > 0 && strcmp(lines[i].text, lines[i - 1].text) == 0)
            continue;
        printf("%s\n", lines[i].text);
    }

    for (int i = 0; i < count; i++)
        free(lines[i].text);
    free(lines);
    return rc;
}

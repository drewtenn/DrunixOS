/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * cut.c — user-space cut utility.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#define MODE_NONE 0
#define MODE_CHARS 1
#define MODE_FIELDS 2

typedef struct
{
    int first;
    int last;   /* 0 means through end */
} range_t;

typedef struct
{
    range_t *ranges;
    int count;
    int cap;
} list_t;

static int parse_uint_part(const char *s, int len, int *out)
{
    int n = 0;
    if (len <= 0)
        return 0;
    for (int i = 0; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        n = n * 10 + (s[i] - '0');
    }
    if (n <= 0)
        return 0;
    *out = n;
    return 1;
}

static int list_add(list_t *list, int first, int last)
{
    if (first <= 0 || (last != 0 && last < first))
        return -1;
    if (list->count >= list->cap)
    {
        int new_cap = list->cap ? list->cap * 2 : 8;
        range_t *new_ranges = (range_t *)realloc(list->ranges,
                                                 sizeof(range_t) * (size_t)new_cap);
        if (!new_ranges)
            return -1;
        list->ranges = new_ranges;
        list->cap = new_cap;
    }
    list->ranges[list->count].first = first;
    list->ranges[list->count].last = last;
    list->count++;
    return 0;
}

static int parse_list(const char *spec, list_t *list)
{
    const char *p = spec;

    if (!spec || !*spec)
        return -1;

    while (*p)
    {
        const char *start = p;
        const char *dash = 0;
        const char *end;
        int first;
        int last;

        while (*p && *p != ',')
        {
            if (*p == '-' && !dash)
                dash = p;
            else if (*p == '-')
                return -1;
            p++;
        }
        end = p;

        if (dash)
        {
            if (dash == start)
            {
                first = 1;
                if (!parse_uint_part(dash + 1, (int)(end - dash - 1), &last))
                    return -1;
            }
            else
            {
                if (!parse_uint_part(start, (int)(dash - start), &first))
                    return -1;
                if (dash + 1 == end)
                    last = 0;
                else if (!parse_uint_part(dash + 1, (int)(end - dash - 1), &last))
                    return -1;
            }
        }
        else
        {
            if (!parse_uint_part(start, (int)(end - start), &first))
                return -1;
            last = first;
        }

        if (list_add(list, first, last) != 0)
            return -1;
        if (*p == ',')
            p++;
    }

    return 0;
}

static int selected(const list_t *list, int pos)
{
    for (int i = 0; i < list->count; i++)
        if (pos >= list->ranges[i].first &&
            (list->ranges[i].last == 0 || pos <= list->ranges[i].last))
            return 1;
    return 0;
}

static void cut_chars(char *line, ssize_t len, const list_t *list)
{
    int had_newline = 0;
    int pos = 1;

    if (len > 0 && line[len - 1] == '\n')
    {
        had_newline = 1;
        line[--len] = '\0';
    }

    for (ssize_t i = 0; i < len; i++, pos++)
        if (selected(list, pos))
            putchar(line[i]);
    if (had_newline)
        putchar('\n');
}

static void cut_fields(char *line, ssize_t len, const list_t *list,
                       char delim, int suppress)
{
    int had_newline = 0;
    int saw_delim = 0;
    int field = 1;
    int printed = 0;
    ssize_t start = 0;

    if (len > 0 && line[len - 1] == '\n')
    {
        had_newline = 1;
        line[--len] = '\0';
    }

    for (ssize_t i = 0; i < len; i++)
        if (line[i] == delim)
        {
            saw_delim = 1;
            break;
        }

    if (!saw_delim)
    {
        if (!suppress)
        {
            fputs(line, stdout);
            if (had_newline)
                putchar('\n');
        }
        return;
    }

    for (ssize_t i = 0; i <= len; i++)
    {
        if (i < len && line[i] != delim)
            continue;

        if (selected(list, field))
        {
            if (printed)
                putchar(delim);
            for (ssize_t j = start; j < i; j++)
                putchar(line[j]);
            printed = 1;
        }

        start = i + 1;
        field++;
    }

    (void)printed;
    if (had_newline)
        putchar('\n');
}

static int process_stream(FILE *f, int mode, const list_t *list,
                          char delim, int suppress)
{
    char *line = 0;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, f)) >= 0)
    {
        if (mode == MODE_FIELDS)
            cut_fields(line, len, list, delim, suppress);
        else
            cut_chars(line, len, list);
    }
    free(line);
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: cut -b list [-n] [file...]\n");
    fprintf(stderr, "       cut -c list [file...]\n");
    fprintf(stderr, "       cut -f list [-d delim] [-s] [file...]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int mode = MODE_NONE;
    int first = 1;
    int suppress = 0;
    char delim = '\t';
    list_t list = { 0, 0, 0 };
    int rc = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1])
    {
        const char *arg = argv[first];
        if (!strcmp(arg, "--"))
        {
            first++;
            break;
        }

        if (arg[1] == 'b' || arg[1] == 'c' || arg[1] == 'f')
        {
            int new_mode = arg[1] == 'f' ? MODE_FIELDS : MODE_CHARS;
            const char *spec = arg[2] ? arg + 2 :
                               (first + 1 < argc ? argv[++first] : 0);
            if (mode != MODE_NONE || !spec || parse_list(spec, &list) != 0)
                return usage();
            mode = new_mode;
        }
        else if (arg[1] == 'd')
        {
            const char *d = arg[2] ? arg + 2 :
                            (first + 1 < argc ? argv[++first] : 0);
            if (!d || !d[0] || d[1])
                return usage();
            delim = d[0];
        }
        else if (!strcmp(arg, "-s"))
        {
            suppress = 1;
        }
        else if (!strcmp(arg, "-n"))
        {
            /* With this ASCII byte/character runtime, -n does not change -b. */
        }
        else
            return usage();
        first++;
    }

    if (mode == MODE_NONE)
        return usage();
    if (suppress && mode != MODE_FIELDS)
        return usage();

    if (first == argc)
        return process_stream(stdin, mode, &list, delim, suppress);

    for (int i = first; i < argc; i++)
    {
        FILE *f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!f)
        {
            fprintf(stderr, "cut: cannot open: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        process_stream(f, mode, &list, delim, suppress);
        if (f != stdin)
            fclose(f);
    }

    free(list.ranges);
    return rc;
}

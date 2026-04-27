/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * grep.c — user-space grep utility.
 */

#include "lib/ctype.h"
#include "lib/stdio.h"
#include "lib/stdlib.h"
#include "lib/string.h"

static int char_eq(char a, char b, int ignore_case)
{
    if (ignore_case)
        return tolower((unsigned char)a) == tolower((unsigned char)b);
    return a == b;
}

static int contains_pattern(const char *line, int line_len,
                            const char *pat, int ignore_case)
{
    int plen = (int)strlen(pat);

    if (plen == 0)
        return 1;

    if (plen > line_len)
        return 0;

    for (int i = 0; i <= line_len - plen; i++)
    {
        int j = 0;
        while (j < plen && char_eq(line[i + j], pat[j], ignore_case))
            j++;
        if (j == plen)
            return 1;
    }
    return 0;
}

static int grep_stream(FILE *f, const char *name, const char *pat,
                       int ignore_case, int show_line_numbers, int invert,
                       int show_filename)
{
    char *line = 0;
    size_t cap = 0;
    ssize_t len;
    unsigned int lineno = 0;
    int matched = 0;

    while ((len = getline(&line, &cap, f)) >= 0)
    {
        int ok;
        lineno++;

        ok = contains_pattern(line, (int)len, pat, ignore_case);
        if (invert)
            ok = !ok;
        if (!ok)
            continue;

        matched = 1;
        if (show_filename && name)
            printf("%s:", name);
        if (show_line_numbers)
            printf("%u:", lineno);
        fwrite(line, 1, (size_t)len, stdout);
        if (len > 0 && line[len - 1] != '\n')
            putchar('\n');
    }

    free(line);
    return matched;
}

int main(int argc, char **argv)
{
    int ignore_case = 0;
    int show_line_numbers = 0;
    int invert = 0;
    int first = 1;
    const char *pat;
    int rc = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1])
    {
        if (!strcmp(argv[first], "--"))
        {
            first++;
            break;
        }
        for (int i = 1; argv[first][i]; i++)
        {
            if (argv[first][i] == 'i')
                ignore_case = 1;
            else if (argv[first][i] == 'n')
                show_line_numbers = 1;
            else if (argv[first][i] == 'v')
                invert = 1;
            else
            {
                fprintf(stderr, "grep: invalid option: %s\n", argv[first]);
                return 2;
            }
        }
        first++;
    }

    if (first >= argc)
    {
        fprintf(stderr, "usage: grep [-inv] pattern [file...]\n");
        return 2;
    }

    pat = argv[first++];
    if (first == argc)
    {
        return grep_stream(stdin, 0, pat, ignore_case, show_line_numbers,
                           invert, 0) ? 0 : 1;
    }

    for (int i = first; i < argc; i++)
    {
        FILE *f;
        if (!strcmp(argv[i], "-"))
            f = stdin;
        else
            f = fopen(argv[i], "r");
        if (!f)
        {
            fprintf(stderr, "grep: cannot open: %s\n", argv[i]);
            rc = 2;
            continue;
        }
        if (grep_stream(f, argv[i], pat, ignore_case, show_line_numbers,
                        invert, argc - first > 1))
        {
            if (rc != 2)
                rc = 0;
        }
        if (f != stdin)
            fclose(f);
    }

    return rc;
}

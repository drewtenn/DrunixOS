/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * uniq.c — user-space uniq utility.
 */

#include "lib/stdio.h"
#include "lib/stdlib.h"
#include "lib/string.h"

static void chomp(char *line, ssize_t *len)
{
    if (*len > 0 && line[*len - 1] == '\n')
    {
        line[*len - 1] = '\0';
        (*len)--;
    }
}

static void print_group(FILE *out, const char *line, unsigned int count,
                        int show_count, int repeated_only, int unique_only)
{
    if (repeated_only && count < 2)
        return;
    if (unique_only && count != 1)
        return;

    if (show_count)
        fprintf(out, "%7u %s\n", count, line);
    else
        fprintf(out, "%s\n", line);
}

int main(int argc, char **argv)
{
    int show_count = 0;
    int repeated_only = 0;
    int unique_only = 0;
    int first = 1;
    FILE *in = stdin;
    FILE *out = stdout;
    char *prev = 0;
    char *line = 0;
    size_t cap = 0;
    ssize_t len;
    unsigned int count = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1])
    {
        for (int i = 1; argv[first][i]; i++)
        {
            if (argv[first][i] == 'c')
                show_count = 1;
            else if (argv[first][i] == 'd')
                repeated_only = 1;
            else if (argv[first][i] == 'u')
                unique_only = 1;
            else
            {
                fprintf(stderr, "uniq: invalid option: %s\n", argv[first]);
                return 2;
            }
        }
        first++;
    }

    if (argc - first > 2)
    {
        fprintf(stderr, "usage: uniq [-cdu] [input [output]]\n");
        return 2;
    }

    if (first < argc && strcmp(argv[first], "-") != 0)
    {
        in = fopen(argv[first], "r");
        if (!in)
        {
            fprintf(stderr, "uniq: cannot open: %s\n", argv[first]);
            return 1;
        }
    }
    if (first + 1 < argc)
    {
        out = fopen(argv[first + 1], "w");
        if (!out)
        {
            fprintf(stderr, "uniq: cannot open: %s\n", argv[first + 1]);
            if (in != stdin)
                fclose(in);
            return 1;
        }
    }

    while ((len = getline(&line, &cap, in)) >= 0)
    {
        chomp(line, &len);
        if (!prev)
        {
            prev = strdup(line);
            if (!prev)
                return 1;
            count = 1;
            continue;
        }

        if (!strcmp(prev, line))
        {
            count++;
        }
        else
        {
            print_group(out, prev, count, show_count, repeated_only, unique_only);
            free(prev);
            prev = strdup(line);
            if (!prev)
                return 1;
            count = 1;
        }
    }

    if (prev)
    {
        print_group(out, prev, count, show_count, repeated_only, unique_only);
        free(prev);
    }
    free(line);

    if (out != stdout)
        fclose(out);
    if (in != stdin)
        fclose(in);
    return 0;
}

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * wc.c — user-space word-count utility.
 */

#include "ctype.h"
#include "stdio.h"
#include "string.h"

typedef struct
{
    unsigned int lines;
    unsigned int words;
    unsigned int bytes;
} counts_t;

static void count_stream(FILE *f, counts_t *c)
{
    char buf[256];
    int in_word = 0;

    for (;;)
    {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
            break;

        c->bytes += (unsigned int)n;
        for (size_t i = 0; i < n; i++)
        {
            unsigned char ch = (unsigned char)buf[i];
            if (ch == '\n')
                c->lines++;

            if (isspace(ch))
                in_word = 0;
            else if (!in_word)
            {
                c->words++;
                in_word = 1;
            }
        }
    }
}

static void print_counts(const counts_t *c, int show_l, int show_w, int show_c,
                         const char *name)
{
    if (show_l)
        printf("%u", c->lines);
    if (show_w)
        printf("%s%u", show_l ? " " : "", c->words);
    if (show_c)
        printf("%s%u", (show_l || show_w) ? " " : "", c->bytes);
    if (name)
        printf(" %s", name);
    putchar('\n');
}

static int parse_options(const char *s, int *show_l, int *show_w, int *show_c)
{
    if (!s || s[0] != '-' || s[1] == '\0')
        return 0;

    for (int i = 1; s[i]; i++)
    {
        if (s[i] == 'l')
            *show_l = 1;
        else if (s[i] == 'w')
            *show_w = 1;
        else if (s[i] == 'c')
            *show_c = 1;
        else
            return -1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int show_l = 0;
    int show_w = 0;
    int show_c = 0;
    int first = 1;
    int rc = 0;
    counts_t total = { 0, 0, 0 };

    while (first < argc)
    {
        int opt = parse_options(argv[first], &show_l, &show_w, &show_c);
        if (opt == 0)
            break;
        if (opt < 0)
        {
            fprintf(stderr, "wc: invalid option: %s\n", argv[first]);
            return 2;
        }
        first++;
    }

    if (!show_l && !show_w && !show_c)
        show_l = show_w = show_c = 1;

    if (first == argc)
    {
        counts_t c = { 0, 0, 0 };
        count_stream(stdin, &c);
        print_counts(&c, show_l, show_w, show_c, 0);
        return 0;
    }

    for (int i = first; i < argc; i++)
    {
        FILE *f;
        counts_t c = { 0, 0, 0 };

        if (!strcmp(argv[i], "-"))
            f = stdin;
        else
            f = fopen(argv[i], "r");

        if (!f)
        {
            fprintf(stderr, "wc: cannot open: %s\n", argv[i]);
            rc = 1;
            continue;
        }

        count_stream(f, &c);
        if (f != stdin)
            fclose(f);

        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
        print_counts(&c, show_l, show_w, show_c, argv[i]);
    }

    if (argc - first > 1)
        print_counts(&total, show_l, show_w, show_c, "total");

    return rc;
}

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * echo.c — user-space echo utility.
 */

#include "stdio.h"
#include "string.h"

int main(int argc, char **argv)
{
    int start = 1;
    int newline = 1;

    if (argc > 1 && !strcmp(argv[1], "-n"))
    {
        newline = 0;
        start = 2;
    }

    for (int i = start; i < argc; i++)
    {
        if (i > start)
            putchar(' ');
        fputs(argv[i], stdout);
    }
    if (newline)
        putchar('\n');

    return 0;
}

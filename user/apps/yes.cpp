/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * yes.c — user-space yes utility.
 */

#include "stdio.h"

int main(int argc, char **argv)
{
    for (;;)
    {
        if (argc == 1)
        {
            puts("y");
        }
        else
        {
            for (int i = 1; i < argc; i++)
            {
                if (i > 1)
                    putchar(' ');
                fputs(argv[i], stdout);
            }
            putchar('\n');
        }
    }
    return 0;
}

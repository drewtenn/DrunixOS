/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * sleep.c — user-space sleep utility.
 */

#include "stdio.h"
#include "unistd.h"

static int parse_seconds(const char *s, unsigned int *out)
{
    unsigned int n = 0;

    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; p++)
    {
        if (*p < '0' || *p > '9')
            return 0;
        n = n * 10u + (unsigned int)(*p - '0');
    }
    *out = n;
    return 1;
}

int main(int argc, char **argv)
{
    unsigned int seconds;

    if (argc != 2 || !parse_seconds(argv[1], &seconds))
    {
        fprintf(stderr, "usage: sleep seconds\n");
        return 2;
    }

    while (seconds > 0)
        seconds = sleep(seconds);

    return 0;
}

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * date.c — user-space date utility.
 */

#include "lib/stdio.h"
#include "lib/time.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    time_t now = time(0);
    if (now == (time_t)-1) {
        fprintf(stderr, "date: cannot read system clock\n");
        return 1;
    }

    struct tm tm;
    if (!localtime_r(&now, &tm)) {
        fprintf(stderr, "date: cannot convert system clock\n");
        return 1;
    }

    char buf[64];
    if (strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm) == 0) {
        fprintf(stderr, "date: output buffer too small\n");
        return 1;
    }

    printf("%s\n", buf);
    return 0;
}

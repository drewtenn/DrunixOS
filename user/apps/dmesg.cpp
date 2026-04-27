/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * dmesg.c — dump the retained kernel log from procfs.
 */

#include "lib/stdio.h"

int main(int argc, char **argv)
{
    FILE *f;
    char buf[512];
    char path[] = "/proc/kmsg";

    (void)argv;

    if (argc != 1) {
        fprintf(stderr, "usage: dmesg\n");
        return 1;
    }

    /*
     * Keep the pathname in writable stack storage so procfs access does not
     * depend on image-range VMA handling.
     */
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "dmesg: cannot open /proc/kmsg\n");
        return 1;
    }

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
            break;
        if (fwrite(buf, 1, n, stdout) != n) {
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

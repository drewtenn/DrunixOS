/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * reader.c — pipe test: reads from stdin (fd 0) and prints to stdout.
 *
 * Reads in a loop until EOF, echoes each chunk to stdout, then prints the
 * total byte count so you can verify the data made it through the pipe
 * intact.  With the libc in place, the echo path uses fwrite(stdout) so
 * the whole program flows through the fd table and respects redirection.
 */

#include "stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[256];
    int total = 0;

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), stdin);
        if (n == 0) break;
        fwrite(buf, 1, n, stdout);
        total += (int)n;
    }

    printf("[reader] received %d bytes\n", total);
    return 0;
}

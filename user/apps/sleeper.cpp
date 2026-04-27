/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * sleeper.c — foreground/background test app.
 *
 * Prints the current iteration, then sleeps for one second.  This is
 * useful for exercising Ctrl+Z, bg, and fg on a process that blocks in
 * the kernel.
 */

#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for (unsigned int i = 1; i <= 100; i++) {
        printf("[sleeper] %u\n", i);
        sleep(1);
    }
    return 0;
}

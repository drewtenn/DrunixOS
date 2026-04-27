/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * hello.c — minimal ring-3 user program for the OS.
 *
 * Build:
 *   See user/Makefile — compiled with -ffreestanding -nostdlib and linked
 *   with lib/crt0.o and the rest of the user runtime via user.ld.
 */

#include "stdio.h"

int main(int argc, char **argv)
{
    printf("Hello from ring 3!\n");
    printf("ELF loader and syscalls work.\n");

    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d]=%s\n", i, argv[i]);

    return 0;
}

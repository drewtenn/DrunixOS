/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * writer.c — pipe test: writes a message to stdout.
 *
 * With the libc in place, fputs(msg, stdout) formats into the stdout
 * FILE* wrapper, which sys_fwrite(1, ...)'s the bytes into either the
 * VGA console or — when the shell has dup2'd stdout onto a pipe's write
 * end — the pipe buffer.  No special path for the pipeline case.
 */

#include "lib/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fputs("hello from the pipe writer\n", stdout);
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef CORE_H
#define CORE_H

#include "process.h"

/*
 * core_dump_process: write a minimal ELF32 core file for `proc`.
 *
 * The dump contains one NT_PRSTATUS note plus PT_LOAD segments for all
 * present user pages in the process's address space.
 *
 * Returns 0 on success, -1 on failure.
 */
int core_dump_process(process_t *proc, int signum);

#endif

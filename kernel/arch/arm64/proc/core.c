/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * core.c - ARM64 core dump hook.
 */

#include "core.h"

int core_dump_process(process_t *proc, int signum)
{
	(void)proc;
	(void)signum;
	return -1;
}

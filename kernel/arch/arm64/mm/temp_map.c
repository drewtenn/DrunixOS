/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mmu.h"
#include <stdint.h>

void *arm64_temp_map(uint64_t phys_addr)
{
	if (phys_addr > UINT32_MAX)
		return 0;

	return (void *)(uintptr_t)phys_addr;
}

void arm64_temp_unmap(void *ptr)
{
	(void)ptr;
}

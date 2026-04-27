/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * nanojpeg_shim.c — libc replacements that NanoJPEG asks for when
 * compiled with NJ_USE_LIBC=0.
 *
 * The shim keeps the vendored nanojpeg.c byte-for-byte unchanged
 * (a license requirement) while routing its allocations and memory
 * primitives through DrunixOS's user-space libc.
 */

#include "malloc.h"
#include "string.h"

void *njAllocMem(int size)
{
	if (size <= 0)
		return 0;
	return malloc((size_t)size);
}

void njFreeMem(void *block)
{
	free(block);
}

void njFillMem(void *block, unsigned char value, int count)
{
	if (count <= 0)
		return;
	memset(block, value, (size_t)count);
}

void njCopyMem(void *dest, const void *src, int count)
{
	if (count <= 0)
		return;
	memcpy(dest, src, (size_t)count);
}

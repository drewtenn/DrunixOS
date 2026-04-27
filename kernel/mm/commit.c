/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "commit.h"
#include "pmm.h"

static int vm_commit_pages_for_bytes(uint32_t bytes, uint32_t *pages_out)
{
	uint32_t rounded;

	if (!pages_out)
		return -1;
	if (bytes == 0) {
		*pages_out = 0;
		return 0;
	}
	if (bytes > UINT32_MAX - (PAGE_SIZE - 1u))
		return -1;

	rounded = bytes + PAGE_SIZE - 1u;
	*pages_out = rounded / PAGE_SIZE;
	return 0;
}

int vm_commit_reserve(process_t *proc, uint32_t bytes)
{
	uint32_t pages;
	uint32_t free_pages;

	if (!proc || !proc->as)
		return -1;
	if (vm_commit_pages_for_bytes(bytes, &pages) != 0)
		return -1;
	if (pages == 0)
		return 0;

	free_pages = pmm_free_page_count();
	if (proc->as->committed_pages > UINT32_MAX - pages)
		return -1;
	if (proc->as->committed_pages + pages > free_pages)
		return -1;

	proc->as->committed_pages += pages;
	return 0;
}

void vm_commit_release(process_t *proc, uint32_t bytes)
{
	uint32_t pages;

	if (!proc || !proc->as)
		return;
	if (vm_commit_pages_for_bytes(bytes, &pages) != 0)
		return;
	if (pages > proc->as->committed_pages)
		proc->as->committed_pages = 0;
	else
		proc->as->committed_pages -= pages;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "elf.h"

int elf_load_file(vfs_file_ref_t file_ref,
                  arch_aspace_t aspace,
                  uintptr_t *entry_out,
                  uintptr_t *image_start_out,
                  uintptr_t *heap_start_out)
{
	return arch_elf_load_user_image(
	    file_ref, aspace, entry_out, image_start_out, heap_start_out);
}

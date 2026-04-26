/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_X86_BOOT_FRAMEBUFFER_MULTIBOOT_H
#define KERNEL_ARCH_X86_BOOT_FRAMEBUFFER_MULTIBOOT_H

#include "framebuffer.h"
#include "pmm.h"

int framebuffer_info_from_multiboot(const multiboot_info_t *mbi,
                                    framebuffer_info_t *out);

#endif

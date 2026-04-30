/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_RAMFB_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_RAMFB_H

/*
 * Phase 1 M2.5a: bring up the QEMU ramfb framebuffer on PLATFORM=virt.
 *
 * arm64_virt_ramfb_init() expects fwcfg_init() to have already been
 * called and to have detected the device. It locates "etc/ramfb",
 * submits a 1024×768×32 XRGB8888 RAMFBCfg pointing at the carve-out
 * recorded by platform_ram_layout(), drops the kernel's Normal-WB alias
 * over the framebuffer (replacing it with Normal-NC), zeroes the
 * framebuffer, and registers /dev/fb0 + /dev/fb0info via fbdev_init.
 *
 * Returns 0 on success and after a benign "ramfb absent" log when
 * fw_cfg has no etc/ramfb entry (e.g. KTEST builds with -display none
 * and no -device ramfb). Returns -1 on hard failures the caller should
 * not silently swallow (DMA error, fbdev_init refuses).
 */
int arm64_virt_ramfb_init(void);

#endif

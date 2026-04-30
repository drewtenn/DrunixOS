/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_FWCFG_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_FWCFG_H

#include <stdint.h>

/*
 * Phase 1 M2.5a: minimal QEMU fw_cfg DMA driver for the arm64 virt
 * machine. Implements selector probe + file-directory walk + DMA
 * read/write so M2.5a's ramfb path can submit a RAMFBCfg blob to
 * `etc/ramfb`. fw_cfg sits at MMIO 0x09020000 on -M virt, and its
 * registers are big-endian regardless of the guest endian.
 *
 * Returns 0 on success, negative on transport / device errors.
 */

#define FWCFG_NAME_MAX 56u

int fwcfg_init(void);
int fwcfg_present(void);

/*
 * Walk the fw_cfg file directory (selector 0x0019) and find an entry
 * by exact name (e.g. "etc/ramfb"). Writes the entry's selector and
 * size on success. Returns 0 on success, -1 if the entry is absent.
 */
int fwcfg_find_file(const char *name,
                    uint16_t *selector_out,
                    uint32_t *size_out);

/*
 * Issue a fw_cfg DMA write of `len` bytes from `src` to the device-side
 * buffer addressed by `selector`. `src` need not be aligned; the helper
 * stages the bytes through a 16-byte-aligned static descriptor.
 *
 * Returns 0 on success, -1 if fw_cfg signalled ERR or the request
 * timed out.
 */
int fwcfg_dma_write(uint16_t selector, const void *src, uint32_t len);

#endif

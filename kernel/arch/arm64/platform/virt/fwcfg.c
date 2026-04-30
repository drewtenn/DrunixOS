/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fwcfg.c - QEMU fw_cfg DMA driver for the arm64 virt machine.
 *
 * Phase 1 M2.5a. Used today by ramfb.c to ship a RAMFBCfg blob to QEMU
 * so the virt display front-end picks up the guest-allocated framebuffer.
 *
 * Spec reference: docs/specs/fw_cfg.txt in the QEMU tree. The arm64 virt
 * memory map fixes the MMIO base at 0x09020000:
 *   +0x00 .. 0x07  data port (8 bytes; per-byte access streams a key)
 *   +0x08 .. 0x09  selector (be16)
 *   +0x0A .. 0x0F  reserved
 *   +0x10 .. 0x17  DMA address (be64; address of FWCfgDmaAccess)
 *
 * fw_cfg fields, including the FWCfgDmaAccess descriptor, are big-endian
 * regardless of guest endian. We byte-swap on the way in and out.
 */

#include "../platform.h"
#include "fwcfg.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdint.h>

#define FWCFG_BASE        0x09020000UL
#define FWCFG_DATA        (FWCFG_BASE + 0x00u)
#define FWCFG_SELECTOR    (FWCFG_BASE + 0x08u)
#define FWCFG_DMA_ADDR    (FWCFG_BASE + 0x10u)

#define FWCFG_KEY_SIGNATURE 0x0000u
#define FWCFG_KEY_ID        0x0001u
#define FWCFG_KEY_FILE_DIR  0x0019u

/* FWCfgDmaAccess.control bits (big-endian on the wire). */
#define FWCFG_DMA_CTL_ERR    (1u << 0)
#define FWCFG_DMA_CTL_READ   (1u << 1)
#define FWCFG_DMA_CTL_SKIP   (1u << 2)
#define FWCFG_DMA_CTL_SELECT (1u << 3)
#define FWCFG_DMA_CTL_WRITE  (1u << 4)

/* Optional ID feature flags (selector 0x0001, big-endian on wire). */
#define FWCFG_ID_VERSION_BIT (1u << 0)
#define FWCFG_ID_DMA_BIT     (1u << 1)

/* fw_cfg DMA spin bound. The transport completes in low microseconds for
 * the small payloads we use; cap at a couple million iterations to keep
 * a wedged QEMU from hanging the boot. */
#define FWCFG_DMA_SPIN_MAX 2000000u

typedef struct {
	uint32_t control_be32;
	uint32_t length_be32;
	uint64_t address_be64;
} __attribute__((packed)) fwcfg_dma_access_t;

typedef struct {
	uint32_t size_be32;
	uint16_t select_be16;
	uint16_t reserved_be16;
	char name[FWCFG_NAME_MAX];
} __attribute__((packed)) fwcfg_file_entry_t;

/* DMA descriptor must be 16-byte aligned per spec. Single-shot serial
 * use; static avoids the absent kheap_alloc_aligned helper. */
static fwcfg_dma_access_t g_dma __attribute__((aligned(16)));

static int g_fwcfg_present;

static inline uint32_t bswap32(uint32_t v)
{
	return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	       ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static inline uint64_t bswap64(uint64_t v)
{
	return ((uint64_t)bswap32((uint32_t)v) << 32) |
	       (uint64_t)bswap32((uint32_t)(v >> 32));
}

static inline uint32_t cpu_to_be32(uint32_t v)
{
	return bswap32(v); /* DrunixOS arm64 is little-endian */
}

static inline uint32_t be32_to_cpu(uint32_t v)
{
	return bswap32(v);
}

static inline uint16_t cpu_to_be16(uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t be16_to_cpu(uint16_t v)
{
	return cpu_to_be16(v);
}

static inline uint64_t cpu_to_be64(uint64_t v)
{
	return bswap64(v);
}

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_write16(uintptr_t addr, uint16_t val)
{
	*(volatile uint16_t *)addr = val;
}

static inline void mmio_write64(uintptr_t addr, uint64_t val)
{
	*(volatile uint64_t *)addr = val;
}

static inline uint8_t mmio_read8(uintptr_t addr)
{
	return *(volatile uint8_t *)addr;
}

/* Select a fw_cfg key. Selector register is be16. */
static void fwcfg_select(uint16_t selector)
{
	mmio_write16(FWCFG_SELECTOR, cpu_to_be16(selector));
}

/* Read 4 raw bytes from the data port. Each mmio_read8 advances the
 * device cursor by 1. The ordering of the resulting bytes is determined
 * by the key being read; per QEMU's fw_cfg spec, the signature is plain
 * ASCII and the ID is little-endian uint32_t. Callers reassemble. */
static void fwcfg_read_data_bytes(uint8_t *out, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		out[i] = mmio_read8(FWCFG_DATA);
}

/*
 * Issue one fw_cfg DMA operation.
 *
 *   control_bits    one or more of FWCFG_DMA_CTL_{READ,WRITE,SKIP}.
 *   want_select     when nonzero, OR in FWCFG_DMA_CTL_SELECT and the
 *                   selector. When zero, the operation continues from
 *                   wherever the device cursor currently sits — useful
 *                   for streaming sequential entries from a
 *                   variable-length list (file directory).
 *   selector        only consulted when want_select is set.
 */
static int fwcfg_dma_run(uint32_t control_bits,
                         int want_select,
                         uint16_t selector,
                         void *buf,
                         uint32_t len)
{
	uint32_t control;
	uint32_t control_word;
	uint32_t spins = 0;

	if (len > 0 && !buf &&
	    (control_bits & (FWCFG_DMA_CTL_READ | FWCFG_DMA_CTL_WRITE)) != 0)
		return -1;

	control_word = control_bits;
	if (want_select)
		control_word |=
		    FWCFG_DMA_CTL_SELECT | ((uint32_t)selector << 16);

	g_dma.control_be32 = cpu_to_be32(control_word);
	g_dma.length_be32 = cpu_to_be32(len);
	g_dma.address_be64 =
	    len && buf ? cpu_to_be64((uint64_t)(uintptr_t)buf) : 0ull;

	/*
	 * Memory barrier: ensure the descriptor stores complete before the
	 * device sees the address write. fw_cfg MMIO is mapped Device on
	 * arm64 (slot 0), so the MMIO write itself is non-bufferable; the
	 * dsb here orders the prior Normal-WB stores against the device
	 * write that triggers the DMA.
	 */
	__asm__ volatile("dsb sy" ::: "memory");
	mmio_write64(FWCFG_DMA_ADDR, cpu_to_be64((uint64_t)(uintptr_t)&g_dma));

	for (;;) {
		__asm__ volatile("dsb sy" ::: "memory");
		control = be32_to_cpu(g_dma.control_be32);
		/* Operation complete when SELECT/READ/SKIP/WRITE all clear. */
		if ((control & (FWCFG_DMA_CTL_SELECT | FWCFG_DMA_CTL_READ |
		                FWCFG_DMA_CTL_SKIP | FWCFG_DMA_CTL_WRITE)) == 0)
			break;
		spins++;
		if (spins > FWCFG_DMA_SPIN_MAX)
			return -1;
	}

	if (control & FWCFG_DMA_CTL_ERR)
		return -1;
	return 0;
}

int fwcfg_init(void)
{
	uint8_t sig[4];
	uint8_t id_bytes[4];
	uint32_t id;
	char line[64];

	g_fwcfg_present = 0;

	fwcfg_select(FWCFG_KEY_SIGNATURE);
	fwcfg_read_data_bytes(sig, sizeof(sig));
	if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
		k_snprintf(line,
		           sizeof(line),
		           "fw_cfg: signature mismatch %c%c%c%c; absent\n",
		           sig[0], sig[1], sig[2], sig[3]);
		platform_uart_puts(line);
		return -1;
	}

	/* ID is little-endian uint32_t per QEMU's fw_cfg spec. */
	fwcfg_select(FWCFG_KEY_ID);
	fwcfg_read_data_bytes(id_bytes, sizeof(id_bytes));
	id = (uint32_t)id_bytes[0] |
	     ((uint32_t)id_bytes[1] << 8) |
	     ((uint32_t)id_bytes[2] << 16) |
	     ((uint32_t)id_bytes[3] << 24);
	if ((id & FWCFG_ID_DMA_BIT) == 0) {
		k_snprintf(line,
		           sizeof(line),
		           "fw_cfg: ID 0x%X lacks DMA capability; absent\n",
		           (unsigned int)id);
		platform_uart_puts(line);
		return -1;
	}

	platform_uart_puts("fw_cfg: signature OK, DMA capable\n");
	g_fwcfg_present = 1;
	return 0;
}

int fwcfg_present(void)
{
	return g_fwcfg_present;
}

int fwcfg_find_file(const char *name,
                    uint16_t *selector_out,
                    uint32_t *size_out)
{
	uint32_t count_be = 0;
	uint32_t count;
	fwcfg_file_entry_t entry;

	if (!g_fwcfg_present || !name)
		return -1;

	/* SELECT FW_CFG_FILE_DIR and read the be32 entry count from the
	 * head of the stream. The device cursor advances to 4 after this. */
	if (fwcfg_dma_run(FWCFG_DMA_CTL_READ, 1, FWCFG_KEY_FILE_DIR,
	                  &count_be, sizeof(count_be)) != 0)
		return -1;
	count = be32_to_cpu(count_be);
	if (count == 0)
		return -1;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t name_len;

		/* No SELECT: continue from the device cursor so each iter
		 * reads the next 64-byte entry. */
		if (fwcfg_dma_run(FWCFG_DMA_CTL_READ, 0, 0,
		                  &entry, sizeof(entry)) != 0)
			return -1;

		name_len = 0;
		while (name_len < FWCFG_NAME_MAX &&
		       entry.name[name_len] != '\0')
			name_len++;
		if (name_len == k_strlen(name) &&
		    k_memcmp(entry.name, name, name_len) == 0) {
			if (selector_out)
				*selector_out = be16_to_cpu(entry.select_be16);
			if (size_out)
				*size_out = be32_to_cpu(entry.size_be32);
			return 0;
		}
	}
	return -1;
}

int fwcfg_dma_write(uint16_t selector, const void *src, uint32_t len)
{
	if (!g_fwcfg_present)
		return -1;
	if (len > 0 && !src)
		return -1;
	/* fwcfg_dma_run takes a non-const buf; cast away const since
	 * WRITE only reads from it. */
	return fwcfg_dma_run(FWCFG_DMA_CTL_WRITE, 1, selector,
	                     (void *)src, len);
}

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mmu.c - AArch64 EL1 stage-1 page table management.
 */

#include "mmu.h"
#include "pmm.h"
#include "../../../platform/platform.h"
#include "kstring.h"
#include <stdint.h>

#define ARM64_MMU_ENTRIES 512u
#define ARM64_MMU_VA_BITS 32u
#define ARM64_MMU_T0SZ (64u - ARM64_MMU_VA_BITS)

#define ARM64_MMU_L1_SHIFT 30u
#define ARM64_MMU_L2_SHIFT 21u
#define ARM64_MMU_L3_SHIFT 12u
#define ARM64_MMU_L1_BLOCK_SIZE (1u << ARM64_MMU_L1_SHIFT)
#define ARM64_MMU_L2_BLOCK_SIZE (1u << ARM64_MMU_L2_SHIFT)

#define ARM64_MMU_DESC_VALID (1ull << 0)
#define ARM64_MMU_DESC_TABLE (1ull << 1)
#define ARM64_MMU_DESC_ATTR_NORMAL (0ull << 2)
#define ARM64_MMU_DESC_ATTR_DEVICE (1ull << 2)
#define ARM64_MMU_DESC_AP_USER (1ull << 6)
#define ARM64_MMU_DESC_AP_RO (1ull << 7)
#define ARM64_MMU_DESC_SH_INNER (3ull << 8)
#define ARM64_MMU_DESC_AF (1ull << 10)
#define ARM64_MMU_DESC_NG (1ull << 11)
#define ARM64_MMU_DESC_PXN (1ull << 53)
#define ARM64_MMU_DESC_UXN (1ull << 54)
#define ARM64_MMU_DESC_SW_COW (1ull << 55)

#define ARM64_MMU_OUTPUT_ADDR_MASK 0x0000FFFFFFFFF000ull
#define ARM64_MMU_ATTR_INDEX_MASK (7ull << 2)

#define ARM64_MMU_MAIR_EL1 0x00000000000004FFull
#define ARM64_MMU_TCR_EL1                                                      \
	((uint64_t)ARM64_MMU_T0SZ | (1ull << 8) | (1ull << 10) | (3ull << 12) |    \
	 (1ull << 23))

typedef uint64_t arm64_mmu_desc_t;

static arm64_mmu_desc_t g_kernel_l1[ARM64_MMU_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static arm64_mmu_desc_t g_kernel_l2_low[ARM64_MMU_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static arch_aspace_t g_current_aspace;
static int g_arm64_mmu_initialized;

static uint32_t arm64_mmu_l1_index(uintptr_t virt)
{
	return ((uint64_t)virt >> ARM64_MMU_L1_SHIFT) & 0x1FFu;
}

static uint32_t arm64_mmu_l2_index(uintptr_t virt)
{
	return ((uint64_t)virt >> ARM64_MMU_L2_SHIFT) & 0x1FFu;
}

static uint32_t arm64_mmu_l3_index(uintptr_t virt)
{
	return ((uint64_t)virt >> ARM64_MMU_L3_SHIFT) & 0x1FFu;
}

static arm64_mmu_desc_t *arm64_mmu_l1(arch_aspace_t aspace)
{
	return (arm64_mmu_desc_t *)(uintptr_t)aspace;
}

static int arm64_mmu_desc_valid(arm64_mmu_desc_t desc)
{
	return (desc & ARM64_MMU_DESC_VALID) != 0;
}

static int arm64_mmu_desc_table(arm64_mmu_desc_t desc)
{
	return (desc & (ARM64_MMU_DESC_VALID | ARM64_MMU_DESC_TABLE)) ==
	       (ARM64_MMU_DESC_VALID | ARM64_MMU_DESC_TABLE);
}

static int arm64_mmu_desc_leaf(arm64_mmu_desc_t desc, uint32_t level)
{
	if (!arm64_mmu_desc_valid(desc))
		return 0;
	if (level == 3)
		return (desc & ARM64_MMU_DESC_TABLE) != 0;
	return (desc & ARM64_MMU_DESC_TABLE) == 0;
}

static arm64_mmu_desc_t *arm64_mmu_table_ptr(arm64_mmu_desc_t desc)
{
	return (arm64_mmu_desc_t *)(uintptr_t)(desc & ARM64_MMU_OUTPUT_ADDR_MASK);
}

static arm64_mmu_desc_t arm64_mmu_table_desc(uintptr_t phys)
{
	return ((uint64_t)phys & ARM64_MMU_OUTPUT_ADDR_MASK) |
	       ARM64_MMU_DESC_VALID | ARM64_MMU_DESC_TABLE;
}

static uint64_t arm64_mmu_leaf_base(arm64_mmu_desc_t desc, uint32_t level)
{
	uint64_t mask = ARM64_MMU_OUTPUT_ADDR_MASK;

	if (level == 1)
		mask &= ~(uint64_t)(ARM64_MMU_L1_BLOCK_SIZE - 1u);
	else if (level == 2)
		mask &= ~(uint64_t)(ARM64_MMU_L2_BLOCK_SIZE - 1u);
	return desc & mask;
}

static uint32_t arm64_mmu_leaf_size(uint32_t level)
{
	if (level == 1)
		return ARM64_MMU_L1_BLOCK_SIZE;
	if (level == 2)
		return ARM64_MMU_L2_BLOCK_SIZE;
	return PAGE_SIZE;
}

static int arm64_mmu_desc_device(arm64_mmu_desc_t desc)
{
	return (desc & ARM64_MMU_ATTR_INDEX_MASK) == ARM64_MMU_DESC_ATTR_DEVICE;
}

static arm64_mmu_desc_t
arm64_mmu_kernel_leaf_desc(uint64_t phys, uint32_t level, int device)
{
	arm64_mmu_desc_t desc;
	uint64_t size = arm64_mmu_leaf_size(level);

	desc = (phys & ARM64_MMU_OUTPUT_ADDR_MASK & ~(size - 1u)) |
	       ARM64_MMU_DESC_VALID | ARM64_MMU_DESC_AF;
	if (level == 3)
		desc |= ARM64_MMU_DESC_TABLE;

	if (device) {
		desc |= ARM64_MMU_DESC_ATTR_DEVICE | ARM64_MMU_DESC_PXN |
		        ARM64_MMU_DESC_UXN;
	} else {
		desc |= ARM64_MMU_DESC_ATTR_NORMAL | ARM64_MMU_DESC_SH_INNER |
		        ARM64_MMU_DESC_UXN;
	}

	return desc;
}

static arm64_mmu_desc_t arm64_mmu_page_desc_from_flags(uint64_t phys,
                                                       uint32_t flags)
{
	arm64_mmu_desc_t desc = (phys & ARM64_MMU_OUTPUT_ADDR_MASK) |
	                        ARM64_MMU_DESC_VALID | ARM64_MMU_DESC_TABLE |
	                        ARM64_MMU_DESC_AF | ARM64_MMU_DESC_ATTR_NORMAL |
	                        ARM64_MMU_DESC_SH_INNER;

	if (flags & ARCH_MM_MAP_USER)
		desc |= ARM64_MMU_DESC_AP_USER | ARM64_MMU_DESC_NG | ARM64_MMU_DESC_PXN;

	if ((flags & ARCH_MM_MAP_WRITE) == 0 || (flags & ARCH_MM_MAP_COW))
		desc |= ARM64_MMU_DESC_AP_RO;
	if ((flags & ARCH_MM_MAP_EXEC) == 0)
		desc |= ARM64_MMU_DESC_UXN;
	if (flags & ARCH_MM_MAP_COW)
		desc |= ARM64_MMU_DESC_SW_COW;

	return desc;
}

static uint32_t arm64_mmu_arch_flags_from_desc(arm64_mmu_desc_t desc)
{
	uint32_t flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ;

	if ((desc & ARM64_MMU_DESC_AP_RO) == 0 &&
	    (desc & ARM64_MMU_DESC_SW_COW) == 0)
		flags |= ARCH_MM_MAP_WRITE;
	if (desc & ARM64_MMU_DESC_AP_USER)
		flags |= ARCH_MM_MAP_USER;
	if (desc & ARM64_MMU_DESC_SW_COW)
		flags |= ARCH_MM_MAP_COW;
	if ((desc & ARM64_MMU_DESC_UXN) == 0)
		flags |= ARCH_MM_MAP_EXEC;

	return flags;
}

static void arm64_mmu_tlb_flush_all(void)
{
	__asm__ volatile("dsb ishst\n"
	                 "tlbi vmalle1\n"
	                 "dsb ish\n"
	                 "isb\n"
	                 :
	                 :
	                 : "memory");
}

static void arm64_mmu_write_ttbr0(uint64_t ttbr0)
{
	__asm__ volatile("dsb ishst\n"
	                 "msr ttbr0_el1, %0\n"
	                 "isb\n"
	                 "tlbi vmalle1\n"
	                 "dsb ish\n"
	                 "isb\n"
	                 :
	                 : "r"(ttbr0)
	                 : "memory");
}

static void arm64_mmu_enable_kernel_table(void)
{
	uint64_t sctlr;

	__asm__ volatile("msr mair_el1, %0" : : "r"((uint64_t)ARM64_MMU_MAIR_EL1));
	__asm__ volatile("msr tcr_el1, %0" : : "r"((uint64_t)ARM64_MMU_TCR_EL1));
	arm64_mmu_write_ttbr0((uint64_t)(uintptr_t)g_kernel_l1);

	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= 1ull;
	__asm__ volatile("msr sctlr_el1, %0\n"
	                 "isb\n"
	                 :
	                 : "r"(sctlr)
	                 : "memory");
}

static void arm64_mmu_build_kernel_tables(void)
{
	k_memset(g_kernel_l1, 0, sizeof(g_kernel_l1));
	k_memset(g_kernel_l2_low, 0, sizeof(g_kernel_l2_low));

	for (uint32_t i = 0; i < ARM64_MMU_ENTRIES; i++) {
		uint64_t phys = (uint64_t)i * ARM64_MMU_L2_BLOCK_SIZE;
		int device = phys >= PLATFORM_PERIPHERAL_BASE;

		g_kernel_l2_low[i] = arm64_mmu_kernel_leaf_desc(phys, 2, device);
	}

	g_kernel_l1[0] = arm64_mmu_table_desc((uintptr_t)g_kernel_l2_low);
	g_kernel_l1[1] = arm64_mmu_kernel_leaf_desc(0x40000000ull, 1, 1);
}

static int arm64_mmu_split_l1_block(arm64_mmu_desc_t *slot)
{
	arm64_mmu_desc_t old_desc;
	arm64_mmu_desc_t *l2;
	uint32_t l2_phys;
	uint64_t base;
	int device;

	l2_phys = pmm_alloc_page();
	if (!l2_phys)
		return -1;

	old_desc = *slot;
	base = arm64_mmu_leaf_base(old_desc, 1);
	device = arm64_mmu_desc_device(old_desc);
	l2 = (arm64_mmu_desc_t *)(uintptr_t)l2_phys;
	for (uint32_t i = 0; i < ARM64_MMU_ENTRIES; i++) {
		l2[i] = arm64_mmu_kernel_leaf_desc(
		    base + (uint64_t)i * ARM64_MMU_L2_BLOCK_SIZE, 2, device);
	}

	*slot = arm64_mmu_table_desc(l2_phys);
	return 0;
}

static int arm64_mmu_split_l2_block(arm64_mmu_desc_t *slot)
{
	arm64_mmu_desc_t old_desc;
	arm64_mmu_desc_t *l3;
	uint32_t l3_phys;
	uint64_t base;
	int device;

	l3_phys = pmm_alloc_page();
	if (!l3_phys)
		return -1;

	old_desc = *slot;
	base = arm64_mmu_leaf_base(old_desc, 2);
	device = arm64_mmu_desc_device(old_desc);
	l3 = (arm64_mmu_desc_t *)(uintptr_t)l3_phys;
	for (uint32_t i = 0; i < ARM64_MMU_ENTRIES; i++) {
		l3[i] = arm64_mmu_kernel_leaf_desc(
		    base + (uint64_t)i * PAGE_SIZE, 3, device);
	}

	*slot = arm64_mmu_table_desc(l3_phys);
	return 0;
}

static int arm64_mmu_clone_l2_table(arm64_mmu_desc_t *l1_slot)
{
	arm64_mmu_desc_t *old_l2;
	arm64_mmu_desc_t *new_l2;
	uint32_t new_l2_phys;

	new_l2_phys = pmm_alloc_page();
	if (!new_l2_phys)
		return -1;

	old_l2 = arm64_mmu_table_ptr(*l1_slot);
	new_l2 = (arm64_mmu_desc_t *)(uintptr_t)new_l2_phys;
	k_memcpy(new_l2, old_l2, PAGE_SIZE);
	*l1_slot = arm64_mmu_table_desc(new_l2_phys);
	return 0;
}

static int arm64_mmu_ensure_l3_slot(arch_aspace_t aspace,
                                    uintptr_t virt,
                                    uint32_t flags,
                                    arm64_mmu_desc_t **pte_out)
{
	arm64_mmu_desc_t *l1;
	arm64_mmu_desc_t *l2;
	arm64_mmu_desc_t *l3;
	arm64_mmu_desc_t *l1_slot;
	arm64_mmu_desc_t *l2_slot;
	uint32_t l1i;
	uint32_t l2i;

	if (!aspace || !pte_out)
		return -1;

	l1 = arm64_mmu_l1(aspace);
	l1i = arm64_mmu_l1_index(virt);
	l2i = arm64_mmu_l2_index(virt);
	l1_slot = &l1[l1i];

	if (!arm64_mmu_desc_valid(*l1_slot)) {
		uint32_t l2_phys = pmm_alloc_page();

		if (!l2_phys)
			return -1;
		l2 = (arm64_mmu_desc_t *)(uintptr_t)l2_phys;
		k_memset(l2, 0, PAGE_SIZE);
		*l1_slot = arm64_mmu_table_desc(l2_phys);
	} else if (!arm64_mmu_desc_table(*l1_slot)) {
		if (arm64_mmu_split_l1_block(l1_slot) != 0)
			return -1;
	} else if ((flags & ARCH_MM_MAP_USER) != 0 &&
	           aspace != arm64_mmu_kernel_aspace() &&
	           *l1_slot == g_kernel_l1[l1i]) {
		if (arm64_mmu_clone_l2_table(l1_slot) != 0)
			return -1;
	}

	l2 = arm64_mmu_table_ptr(*l1_slot);
	l2_slot = &l2[l2i];
	if (!arm64_mmu_desc_valid(*l2_slot)) {
		uint32_t l3_phys = pmm_alloc_page();

		if (!l3_phys)
			return -1;
		l3 = (arm64_mmu_desc_t *)(uintptr_t)l3_phys;
		k_memset(l3, 0, PAGE_SIZE);
		*l2_slot = arm64_mmu_table_desc(l3_phys);
	} else if (!arm64_mmu_desc_table(*l2_slot)) {
		if (arm64_mmu_split_l2_block(l2_slot) != 0)
			return -1;
	}

	l3 = arm64_mmu_table_ptr(*l2_slot);
	*pte_out = &l3[arm64_mmu_l3_index(virt)];
	return 0;
}

static int arm64_mmu_lookup_leaf(arch_aspace_t aspace,
                                 uintptr_t virt,
                                 arm64_mmu_desc_t **slot_out,
                                 uint32_t *level_out)
{
	arm64_mmu_desc_t *l1;
	arm64_mmu_desc_t *l2;
	arm64_mmu_desc_t *l3;
	arm64_mmu_desc_t *slot;

	if (!aspace || !slot_out || !level_out)
		return -1;

	l1 = arm64_mmu_l1(aspace);
	slot = &l1[arm64_mmu_l1_index(virt)];
	if (!arm64_mmu_desc_valid(*slot))
		return -1;
	if (arm64_mmu_desc_leaf(*slot, 1)) {
		*slot_out = slot;
		*level_out = 1;
		return 0;
	}

	l2 = arm64_mmu_table_ptr(*slot);
	slot = &l2[arm64_mmu_l2_index(virt)];
	if (!arm64_mmu_desc_valid(*slot))
		return -1;
	if (arm64_mmu_desc_leaf(*slot, 2)) {
		*slot_out = slot;
		*level_out = 2;
		return 0;
	}

	l3 = arm64_mmu_table_ptr(*slot);
	slot = &l3[arm64_mmu_l3_index(virt)];
	if (!arm64_mmu_desc_leaf(*slot, 3))
		return -1;

	*slot_out = slot;
	*level_out = 3;
	return 0;
}

int arm64_mmu_enabled(void)
{
	uint64_t sctlr;

	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	return (sctlr & 1ull) != 0;
}

void arm64_mmu_init(void)
{
	if (g_arm64_mmu_initialized)
		return;

	arm64_mmu_build_kernel_tables();
	g_current_aspace = (arch_aspace_t)(uintptr_t)g_kernel_l1;
	arm64_mmu_enable_kernel_table();
	g_arm64_mmu_initialized = 1;
}

arch_aspace_t arm64_mmu_kernel_aspace(void)
{
	if (!g_arm64_mmu_initialized)
		arm64_mmu_init();

	return (arch_aspace_t)(uintptr_t)g_kernel_l1;
}

arch_aspace_t arm64_mmu_aspace_create(void)
{
	uint32_t l1_phys;
	arm64_mmu_desc_t *l1;

	if (!g_arm64_mmu_initialized)
		arm64_mmu_init();

	l1_phys = pmm_alloc_page();
	if (!l1_phys)
		return 0;

	l1 = (arm64_mmu_desc_t *)(uintptr_t)l1_phys;
	k_memcpy(l1, g_kernel_l1, PAGE_SIZE);
	return (arch_aspace_t)l1_phys;
}

arch_aspace_t arm64_mmu_aspace_clone(arch_aspace_t src)
{
	arch_aspace_t dst;
	arm64_mmu_desc_t *src_l1;

	if (!src)
		return 0;

	dst = arm64_mmu_aspace_create();
	if (!dst)
		return 0;

	src_l1 = arm64_mmu_l1(src);
	for (uint32_t l1i = 0; l1i < ARM64_MMU_ENTRIES; l1i++) {
		arm64_mmu_desc_t *src_l2;

		if (!arm64_mmu_desc_table(src_l1[l1i]))
			continue;

		src_l2 = arm64_mmu_table_ptr(src_l1[l1i]);
		for (uint32_t l2i = 0; l2i < ARM64_MMU_ENTRIES; l2i++) {
			arm64_mmu_desc_t *src_l3;

			if (!arm64_mmu_desc_table(src_l2[l2i]))
				continue;

			src_l3 = arm64_mmu_table_ptr(src_l2[l2i]);
			for (uint32_t l3i = 0; l3i < ARM64_MMU_ENTRIES; l3i++) {
				arm64_mmu_desc_t src_desc = src_l3[l3i];
				uintptr_t virt;
				uint32_t src_flags;
				uint32_t src_phys;
				uint32_t dst_phys;
				void *src_page;
				void *dst_page;

				if (!arm64_mmu_desc_leaf(src_desc, 3) ||
				    (src_desc & ARM64_MMU_DESC_AP_USER) == 0)
					continue;

				src_phys = (uint32_t)arm64_mmu_leaf_base(src_desc, 3);
				dst_phys = pmm_alloc_page();
				if (!dst_phys)
					goto fail;

				src_page = (void *)(uintptr_t)src_phys;
				dst_page = (void *)(uintptr_t)dst_phys;
				k_memcpy(dst_page, src_page, PAGE_SIZE);

				virt = ((uintptr_t)l1i << ARM64_MMU_L1_SHIFT) |
				       ((uintptr_t)l2i << ARM64_MMU_L2_SHIFT) |
				       ((uintptr_t)l3i << ARM64_MMU_L3_SHIFT);
				src_flags = arm64_mmu_arch_flags_from_desc(src_desc);
				if (arm64_mmu_map(dst, virt, dst_phys, src_flags) != 0) {
					pmm_free_page(dst_phys);
					goto fail;
				}
			}
		}
	}

	return dst;

fail:
	arm64_mmu_aspace_destroy(dst);
	return 0;
}

void arm64_mmu_aspace_switch(arch_aspace_t aspace)
{
	if (!aspace)
		aspace = arm64_mmu_kernel_aspace();

	g_current_aspace = aspace;
	arm64_mmu_write_ttbr0((uint64_t)aspace);
}

void arm64_mmu_sync_current_from_identity(void)
{
}

void arm64_mmu_sync_current_to_identity(void)
{
}

void arm64_mmu_aspace_destroy(arch_aspace_t aspace)
{
	arm64_mmu_desc_t *l1;

	if (!aspace || aspace == arm64_mmu_kernel_aspace())
		return;

	if (g_current_aspace == aspace)
		arm64_mmu_aspace_switch(arm64_mmu_kernel_aspace());

	l1 = arm64_mmu_l1(aspace);
	for (uint32_t l1i = 0; l1i < ARM64_MMU_ENTRIES; l1i++) {
		arm64_mmu_desc_t *l2;

		if (!arm64_mmu_desc_table(l1[l1i]) || l1[l1i] == g_kernel_l1[l1i])
			continue;

		l2 = arm64_mmu_table_ptr(l1[l1i]);
		for (uint32_t l2i = 0; l2i < ARM64_MMU_ENTRIES; l2i++) {
			arm64_mmu_desc_t *l3;

			if (!arm64_mmu_desc_table(l2[l2i]))
				continue;

			l3 = arm64_mmu_table_ptr(l2[l2i]);
			for (uint32_t l3i = 0; l3i < ARM64_MMU_ENTRIES; l3i++) {
				if (arm64_mmu_desc_leaf(l3[l3i], 3) &&
				    (l3[l3i] & ARM64_MMU_DESC_AP_USER) != 0)
					pmm_decref((uint32_t)arm64_mmu_leaf_base(l3[l3i], 3));
			}
			pmm_free_page((uint32_t)(uintptr_t)l3);
		}
		pmm_free_page((uint32_t)(uintptr_t)l2);
	}

	pmm_free_page((uint32_t)aspace);
}

int arm64_mmu_map(arch_aspace_t aspace,
                  uintptr_t virt,
                  uint64_t phys,
                  uint32_t flags)
{
	arm64_mmu_desc_t *pte;

	if (!aspace || phys > UINT32_MAX)
		return -1;
	if ((virt & (PAGE_SIZE - 1u)) != 0 || (phys & (PAGE_SIZE - 1u)) != 0)
		return -1;
	if (arm64_mmu_ensure_l3_slot(aspace, virt, flags, &pte) != 0)
		return -1;

	*pte = arm64_mmu_page_desc_from_flags(
	    phys, flags | ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ);
	arm64_mmu_tlb_flush_all();
	return 0;
}

int arm64_mmu_unmap(arch_aspace_t aspace, uintptr_t virt)
{
	arm64_mmu_desc_t *slot;
	uint32_t level;

	if (arm64_mmu_lookup_leaf(aspace, virt, &slot, &level) != 0 || level != 3 ||
	    ((*slot) & ARM64_MMU_DESC_AP_USER) == 0)
		return -1;

	pmm_decref((uint32_t)arm64_mmu_leaf_base(*slot, 3));
	*slot = 0;
	arm64_mmu_tlb_flush_all();
	return 0;
}

int arm64_mmu_query(arch_aspace_t aspace,
                    uintptr_t virt,
                    arch_mm_mapping_t *out)
{
	arm64_mmu_desc_t *slot;
	uint32_t level;
	uint32_t size;
	uint64_t offset;

	if (!out || arm64_mmu_lookup_leaf(aspace, virt, &slot, &level) != 0)
		return -1;

	size = arm64_mmu_leaf_size(level);
	offset = (uint64_t)virt & (uint64_t)(size - 1u);
	out->phys_addr = arm64_mmu_leaf_base(*slot, level) + offset;
	out->flags = arm64_mmu_arch_flags_from_desc(*slot);
	return 0;
}

int arm64_mmu_update(arch_aspace_t aspace,
                     uintptr_t virt,
                     uint32_t clear_flags,
                     uint32_t set_flags)
{
	arm64_mmu_desc_t *slot;
	arm64_mmu_desc_t old_desc;
	uint32_t flags;
	uint32_t level;
	uint32_t unsupported;

	if (arm64_mmu_lookup_leaf(aspace, virt, &slot, &level) != 0 || level != 3)
		return -1;

	unsupported = clear_flags | set_flags;
	if (unsupported & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                   ARCH_MM_MAP_EXEC | ARCH_MM_MAP_USER))
		return -1;

	old_desc = *slot;
	flags = arm64_mmu_arch_flags_from_desc(old_desc);
	flags &= ~clear_flags;
	if (set_flags & ARCH_MM_MAP_COW) {
		flags |= ARCH_MM_MAP_COW;
		flags &= ~(uint32_t)ARCH_MM_MAP_WRITE;
	} else {
		flags |= set_flags;
	}

	*slot =
	    arm64_mmu_page_desc_from_flags(arm64_mmu_leaf_base(old_desc, 3), flags);
	arm64_mmu_tlb_flush_all();
	return 0;
}

void arm64_mmu_invalidate_page(arch_aspace_t aspace, uintptr_t virt)
{
	(void)aspace;
	(void)virt;
	arm64_mmu_tlb_flush_all();
}

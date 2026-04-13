/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * uaccess.c — validated copies between kernel memory and user mappings.
 */

#include "uaccess.h"
#include "paging.h"
#include "pmm.h"
#include "kstring.h"

static void uaccess_invlpg(uint32_t virt)
{
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

static int uaccess_vma_allows(const process_t *proc, uint32_t user_addr,
                              int write_access)
{
    const vm_area_t *vma = vma_find_const(proc, user_addr);

    if (!vma)
        return 0;
    if (write_access)
        return (vma->flags & VMA_FLAG_WRITE) != 0;
    return (vma->flags & (VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC)) != 0;
}

static int uaccess_range_ok(uint32_t user_addr, uint32_t len)
{
    uint32_t end;

    if (len == 0)
        return 0;
    if (user_addr >= USER_STACK_TOP)
        return -1;

    end = user_addr + len;
    if (end < user_addr || end > USER_STACK_TOP)
        return -1;

    return 0;
}

static int uaccess_break_cow(uint32_t *pte, uint32_t fault_page)
{
    uint32_t old_phys = paging_entry_addr(*pte);

    if ((*pte & PG_COW) == 0)
        return -1;

    if (pmm_refcount(old_phys) <= 1) {
        uint32_t flags = (paging_entry_flags(*pte) | PG_WRITABLE) & ~PG_COW;
        *pte = paging_entry_build(old_phys, flags);
        uaccess_invlpg(fault_page);
        return 0;
    }

    uint32_t new_phys = pmm_alloc_page();
    if (!new_phys)
        return -1;

    k_memcpy((void *)new_phys, (const void *)old_phys, PAGE_SIZE);
    {
        uint32_t flags = (paging_entry_flags(*pte) | PG_WRITABLE) & ~PG_COW;
        *pte = paging_entry_build(new_phys, flags);
    }
    uaccess_invlpg(fault_page);
    pmm_decref(old_phys);
    return 0;
}

static int uaccess_translate(process_t *proc, uint32_t user_addr,
                             int write_access, uint8_t **kptr_out)
{
    uint32_t *pd;
    uint32_t *pte;
    uint32_t pdi;
    uint32_t page_base;

    if (!proc || user_addr >= USER_STACK_TOP)
        return -1;
    if (!uaccess_vma_allows(proc, user_addr, write_access))
        return -1;

    pd = (uint32_t *)proc->pd_phys;
    pdi = user_addr >> 22;
    if ((pd[pdi] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
        return -1;

    if (paging_walk(proc->pd_phys, user_addr, &pte) != 0)
        return -1;
    if ((*pte & PG_USER) == 0)
        return -1;

    page_base = user_addr & ~0xFFFu;
    if (write_access && (*pte & PG_WRITABLE) == 0) {
        if (uaccess_break_cow(pte, page_base) != 0)
            return -1;
    }

    *kptr_out = (uint8_t *)(paging_entry_addr(*pte) + (user_addr & 0xFFFu));
    return 0;
}

int uaccess_prepare(process_t *proc, uint32_t user_addr, uint32_t len,
                    int write_access)
{
    uint32_t offset = 0;

    if (uaccess_range_ok(user_addr, len) != 0)
        return -1;

    while (offset < len) {
        uint32_t addr = user_addr + offset;
        uint32_t page_off = addr & 0xFFFu;
        uint32_t chunk = PAGE_SIZE - page_off;
        uint8_t *unused;

        if (chunk > len - offset)
            chunk = len - offset;

        if (uaccess_translate(proc, addr, write_access, &unused) != 0)
            return -1;

        offset += chunk;
    }

    return 0;
}

int uaccess_copy_from_user(process_t *proc, void *dst, uint32_t user_src,
                           uint32_t len)
{
    uint32_t offset = 0;

    if (uaccess_prepare(proc, user_src, len, 0) != 0)
        return -1;

    while (offset < len) {
        uint32_t addr = user_src + offset;
        uint32_t page_off = addr & 0xFFFu;
        uint32_t chunk = PAGE_SIZE - page_off;
        uint8_t *kptr;

        if (chunk > len - offset)
            chunk = len - offset;

        if (uaccess_translate(proc, addr, 0, &kptr) != 0)
            return -1;

        k_memcpy((uint8_t *)dst + offset, kptr, chunk);
        offset += chunk;
    }

    return 0;
}

int uaccess_copy_to_user(process_t *proc, uint32_t user_dst, const void *src,
                         uint32_t len)
{
    uint32_t offset = 0;

    if (uaccess_prepare(proc, user_dst, len, 1) != 0)
        return -1;

    while (offset < len) {
        uint32_t addr = user_dst + offset;
        uint32_t page_off = addr & 0xFFFu;
        uint32_t chunk = PAGE_SIZE - page_off;
        uint8_t *kptr;

        if (chunk > len - offset)
            chunk = len - offset;

        if (uaccess_translate(proc, addr, 1, &kptr) != 0)
            return -1;

        k_memcpy(kptr, (const uint8_t *)src + offset, chunk);
        offset += chunk;
    }

    return 0;
}

int uaccess_copy_string_from_user(process_t *proc, char *dst, uint32_t dstsz,
                                  uint32_t user_src)
{
    uint32_t i;

    if (!proc || !dst || dstsz == 0 || user_src == 0)
        return -1;

    for (i = 0; i < dstsz; i++) {
        uint8_t *kptr;

        if (uaccess_translate(proc, user_src + i, 0, &kptr) != 0)
            return -1;

        dst[i] = (char)*kptr;
        if (dst[i] == '\0')
            return 0;
    }

    return -1;
}

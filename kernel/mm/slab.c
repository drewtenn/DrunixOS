/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * slab.c — fixed-size slab allocator for small kernel objects.
 */

#include "slab.h"
#include "kheap.h"
#include "pmm.h"
#include <stdint.h>

/* Size of the slab header that sits at the base of every slab page. */
#define SLAB_HDR_SZ  ((uint32_t)sizeof(slab_hdr_t))   /* 12 bytes on i386 */

/* ── Internal helpers ────────────────────────────────────────────────────── */

/*
 * slab_format: initialise a fresh PMM page at `page_phys` as a slab for
 * `cache`.  Threads the embedded free list through every object slot in order,
 * so the first alloc returns the lowest-addressed object.
 *
 * Layout inside the page:
 *
 *   [ slab_hdr_t (12 B) | obj0 | obj1 | ... | obj_{N-1} | unused padding ]
 *
 * Since phys == virt in the kernel's identity map, casting the physical
 * address to a pointer is safe for all slab pages (0–128 MB range).
 */
static slab_hdr_t *slab_format(const kmem_cache_t *cache, uint32_t page_phys)
{
    slab_hdr_t *slab = (slab_hdr_t *)page_phys;
    slab->next   = (slab_hdr_t *)0;
    slab->n_free = cache->objs_per_slab;

    uint8_t *base = (uint8_t *)page_phys + SLAB_HDR_SZ;
    for (uint32_t i = 0; i < cache->objs_per_slab; i++) {
        void **slot = (void **)(base + i * cache->obj_size);
        *slot = (i + 1 < cache->objs_per_slab)
                ? (void *)(base + (i + 1) * cache->obj_size)
                : (void *)0;
    }
    slab->free_head = base;
    return slab;
}

/*
 * slab_grow: allocate one PMM page, format it as a slab, and prepend it to
 * the cache's partial list.  Returns the new slab, or NULL on OOM.
 */
static slab_hdr_t *slab_grow(kmem_cache_t *cache)
{
    uint32_t page = pmm_alloc_page();
    if (!page) return (slab_hdr_t *)0;

    slab_hdr_t *slab = slab_format(cache, page);
    slab->next     = cache->partial;
    cache->partial = slab;
    return slab;
}

/*
 * list_remove: unlink `target` from the singly-linked list whose head is at
 * `*head_ptr`.  No-ops silently if target is not in the list.
 */
static void list_remove(slab_hdr_t **head_ptr, slab_hdr_t *target)
{
    slab_hdr_t **p = head_ptr;
    while (*p && *p != target)
        p = &(*p)->next;
    if (*p)
        *p = target->next;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

kmem_cache_t *kmem_cache_create(const char *name, uint32_t obj_size)
{
    /* Objects carry a free-list pointer when free — must be at least 4 B. */
    if (obj_size < sizeof(void *))
        obj_size = (uint32_t)sizeof(void *);

    /* Align to 4 bytes so all objects stay naturally aligned. */
    obj_size = (obj_size + 3u) & ~3u;

    uint32_t objs = (PAGE_SIZE - SLAB_HDR_SZ) / obj_size;
    if (objs == 0)
        return (kmem_cache_t *)0;  /* object too large: won't fit in one page */

    kmem_cache_t *c = (kmem_cache_t *)kmalloc(sizeof(kmem_cache_t));
    if (!c) return (kmem_cache_t *)0;

    c->name          = name;
    c->obj_size      = obj_size;
    c->objs_per_slab = objs;
    c->partial       = (slab_hdr_t *)0;
    c->full          = (slab_hdr_t *)0;
    return c;
}

void *kmem_cache_alloc(kmem_cache_t *cache)
{
    /* Find a partial slab, growing the cache if none exists. */
    slab_hdr_t *slab = cache->partial;
    if (!slab) {
        slab = slab_grow(cache);
        if (!slab) return (void *)0;
    }

    /* Pop the head of the free list. */
    void *obj       = slab->free_head;
    slab->free_head = *(void **)obj;
    slab->n_free--;

    /* Full slab: migrate from partial → full. */
    if (slab->n_free == 0) {
        cache->partial = slab->next;
        slab->next     = cache->full;
        cache->full    = slab;
    }

    return obj;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj)
{
    /*
     * Locate the owning slab by masking the object address down to its page
     * boundary.  This works because:
     *   - Every slab is exactly one 4 KB PMM page.
     *   - The slab_hdr_t sits at the very start of that page.
     *   - Every object in the slab is within the same page
     *     (enforced by kmem_cache_create: objs_per_slab >= 1 and
     *      SLAB_HDR_SZ + objs_per_slab * obj_size <= PAGE_SIZE).
     */
    slab_hdr_t *slab = (slab_hdr_t *)((uint32_t)obj & ~(PAGE_SIZE - 1u));

    int was_full = (slab->n_free == 0);

    /* Push the object back onto the slab's free list. */
    *(void **)obj   = slab->free_head;
    slab->free_head = obj;
    slab->n_free++;

    if (was_full) {
        /* Slab was full: migrate full → partial. */
        list_remove(&cache->full, slab);
        slab->next     = cache->partial;
        cache->partial = slab;
    } else if (slab->n_free == cache->objs_per_slab) {
        /* Slab is now completely empty: remove and return the page to PMM. */
        list_remove(&cache->partial, slab);
        pmm_free_page((uint32_t)slab);
    }
}

void kmem_cache_destroy(kmem_cache_t *cache)
{
    slab_hdr_t *s, *next;

    for (s = cache->partial; s; s = next) {
        next = s->next;
        pmm_free_page((uint32_t)s);
    }
    for (s = cache->full; s; s = next) {
        next = s->next;
        pmm_free_page((uint32_t)s);
    }
    kfree(cache);
}

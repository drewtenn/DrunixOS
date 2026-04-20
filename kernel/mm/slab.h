/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>

/*
 * slab_hdr_t: per-slab metadata stored at the very start of each 4 KB PMM
 * page that backs a slab.  Objects begin immediately after this header at
 * offset sizeof(slab_hdr_t) (12 bytes, 4-byte-aligned).
 *
 * When an object is free, its first four bytes hold a pointer to the next
 * free object in the same slab.  The last free object stores NULL.  No
 * separate free-list array is needed.
 */
typedef struct slab_hdr {
	struct slab_hdr *next; /* intrusive link: partial or full list   */
	uint32_t n_free;       /* number of free objects in this slab    */
	void *free_head;       /* head of the embedded free-object chain */
} slab_hdr_t;              /* 12 bytes on i386                        */

/*
 * kmem_cache_t: descriptor for a fixed-size object pool.
 *
 * The descriptor itself is allocated from kheap (kmalloc) once per cache and
 * lives until kmem_cache_destroy is called.
 *
 * Maximum object size: PAGE_SIZE - sizeof(slab_hdr_t) = 4084 bytes.
 * At least one object must fit per slab page.
 *
 * For objects larger than 4084 bytes use kmalloc / pmm_alloc_page directly.
 */
typedef struct kmem_cache {
	const char *name;       /* human-readable label for debugging     */
	uint32_t obj_size;      /* bytes per object (4-byte-aligned)      */
	uint32_t objs_per_slab; /* objects that fit in one 4 KB slab page */
	slab_hdr_t *partial;    /* slabs with at least one free object    */
	slab_hdr_t *full;       /* slabs with no free objects             */
} kmem_cache_t;

/*
 * kmem_cache_create — allocate and initialise a new object cache.
 *
 *   name:     descriptive label (pointer stored as-is; must outlive the cache).
 *   obj_size: bytes per object.
 *             - Rounded up to the next multiple of 4 internally.
 *             - Must be >= 4 (objects carry a free-list pointer when free).
 *             - Must fit at least one object in a 4 KB page after the header
 *               (i.e. effective size <= PAGE_SIZE - sizeof(slab_hdr_t) = 4084).
 *
 * Returns a pointer to the new kmem_cache_t, or NULL on failure.
 */
kmem_cache_t *kmem_cache_create(const char *name, uint32_t obj_size);

/*
 * kmem_cache_alloc — allocate one object from the cache.
 *
 * Pops a free object from the first partial slab.  If no partial slab exists,
 * a new slab page is obtained from the PMM.
 *
 * Returns a pointer to the object, or NULL on OOM.
 */
void *kmem_cache_alloc(kmem_cache_t *cache);

/*
 * kmem_cache_free — return an object to its cache.
 *
 * Locates the owning slab by page-aligning obj (the slab header lives at the
 * page base).  Pushes the object back onto the slab's free list.  If the slab
 * becomes completely empty, its page is returned to the PMM.
 *
 * obj must have been allocated from this cache via kmem_cache_alloc.
 */
void kmem_cache_free(kmem_cache_t *cache, void *obj);

/*
 * kmem_cache_destroy — release all slab pages and the cache descriptor.
 *
 * All objects must have been freed before calling this, otherwise the PMM
 * pages backing them become inaccessible.
 */
void kmem_cache_destroy(kmem_cache_t *cache);

#endif /* SLAB_H */

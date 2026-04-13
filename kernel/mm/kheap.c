/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kheap.c — general-purpose kernel heap allocator built on top of page mappings.
 */

#include "kheap.h"
#include <stdint.h>

/* Each allocation is preceded by this 16-byte header */
typedef struct heap_block {
    uint32_t            magic;
    uint32_t            size;    /* bytes of user-accessible payload */
    uint32_t            free;    /* 1 = free, 0 = allocated */
    struct heap_block  *next;
} heap_block_t;

/* A new free block is only split off if it can hold a header + this minimum */
#define HEAP_MIN_SPLIT (sizeof(heap_block_t) + 16u)

static heap_block_t *heap_head;

void kheap_init(void) {
    heap_head        = (heap_block_t *)HEAP_START;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size  = HEAP_END - HEAP_START - sizeof(heap_block_t);
    heap_head->free  = 1;
    heap_head->next  = 0;
}

void *kmalloc(uint32_t size) {
    heap_block_t *cur = heap_head;

    while (cur) {
        if (cur->magic != HEAP_MAGIC)
            return 0;   /* heap corruption */

        if (cur->free && cur->size >= size) {
            /* Split block if there is room for a new header + minimum payload */
            if (cur->size >= size + HEAP_MIN_SPLIT) {
                heap_block_t *next = (heap_block_t *)((uint8_t *)(cur + 1) + size);
                next->magic = HEAP_MAGIC;
                next->size  = cur->size - size - sizeof(heap_block_t);
                next->free  = 1;
                next->next  = cur->next;

                cur->size = size;
                cur->next = next;
            }

            cur->free = 0;
            return (void *)(cur + 1);
        }

        cur = cur->next;
    }

    return 0;   /* out of heap space */
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    heap_block_t *block = (heap_block_t *)ptr - 1;

    if (block->magic != HEAP_MAGIC)
        return;   /* bad pointer / corruption — silently ignore */

    block->free = 1;

    /* Coalesce adjacent free blocks in a single forward pass */
    heap_block_t *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(heap_block_t) + cur->next->size;
            cur->next  = cur->next->next;
            /* do not advance cur — check the newly merged block again */
        } else {
            cur = cur->next;
        }
    }
}

uint32_t kheap_free_bytes(void) {
    uint32_t total = 0;
    heap_block_t *cur = heap_head;
    while (cur) {
        if (cur->free)
            total += cur->size;
        cur = cur->next;
    }
    return total;
}

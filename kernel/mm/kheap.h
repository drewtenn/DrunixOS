/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KHEAP_H
#define KHEAP_H

#include <stdint.h>

#define HEAP_START  0x00032000u
#define HEAP_END    0x00090000u   /* do not cross into stack */
#define HEAP_MAGIC  0xDEADBEEFu

void     kheap_init(void);
void    *kmalloc(uint32_t size);
void     kfree(void *ptr);
uint32_t kheap_free_bytes(void);   /* diagnostic: sum of free block payloads */

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KHEAP_H
#define KHEAP_H

#include "arch_layout.h"
#include <stdint.h>

#define HEAP_START ARCH_HEAP_START
#define HEAP_END ARCH_HEAP_END
#define HEAP_MAGIC 0xDEADBEEFu

void kheap_init(void);
void *kmalloc(uint32_t size);
void kfree(void *ptr);
uint32_t kheap_free_bytes(void); /* diagnostic: sum of free block payloads */

#endif

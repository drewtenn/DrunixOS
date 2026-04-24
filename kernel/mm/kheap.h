/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KHEAP_H
#define KHEAP_H

#include <stdint.h>

#ifdef __aarch64__
#define HEAP_START 0x01800000u
#define HEAP_END 0x01F00000u
#else
#define HEAP_START 0x00032000u
#define HEAP_END 0x0009F000u /* stops below the VGA/ROM hole at 0xA0000 */
#endif
#define HEAP_MAGIC 0xDEADBEEFu

void kheap_init(void);
void *kmalloc(uint32_t size);
void kfree(void *ptr);
uint32_t kheap_free_bytes(void); /* diagnostic: sum of free block payloads */

#endif

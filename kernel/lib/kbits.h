/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KBITS_H
#define KBITS_H

#include <stdint.h>

/*
 * kbits — inline bit-manipulation utilities for uint32_t bitmaps.
 *
 * `map` is a pointer to an array of uint32_t words.
 * `bit` is the zero-based bit index across the full array.
 *
 * k_find_first_zero_bit scans [0, nbits) and returns the index of the first
 * zero bit, or -1 if all bits in the range are set.
 */

static inline void k_set_bit(uint32_t *map, uint32_t bit)
{
    map[bit >> 5] |= (1u << (bit & 31u));
}

static inline void k_clear_bit(uint32_t *map, uint32_t bit)
{
    map[bit >> 5] &= ~(1u << (bit & 31u));
}

static inline int k_test_bit(const uint32_t *map, uint32_t bit)
{
    return (int)((map[bit >> 5] >> (bit & 31u)) & 1u);
}

static inline int32_t k_find_first_zero_bit(const uint32_t *map, uint32_t nbits)
{
    uint32_t words = (nbits + 31u) >> 5u;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t inv = ~map[w];   /* bits that are zero in the original */
        if (inv) {
            /* __builtin_ctz gives the index of the lowest set bit. */
            uint32_t bit = (w << 5u) + (uint32_t)__builtin_ctz(inv);
            return (int32_t)((bit < nbits) ? bit : -1);
        }
    }
    return -1;
}

#endif

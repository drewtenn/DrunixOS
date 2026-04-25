/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef STDINT_H
#define STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;

#define UINT32_MAX 4294967295u
#if __SIZEOF_POINTER__ == 8
#define UINTPTR_MAX 18446744073709551615ull
#else
#define UINTPTR_MAX UINT32_MAX
#endif

#endif

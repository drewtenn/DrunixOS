/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ARM64_INIT_LAYOUT_H
#define ARM64_INIT_LAYOUT_H

#include <stdint.h>

#define ARM64_INIT_IMAGE_BASE ((uintptr_t)0x02000000u)
#define ARM64_INIT_IMAGE_LIMIT ((uintptr_t)0x02800000u)
#define ARM64_INIT_STACK_BASE ((uintptr_t)0x03000000u)
#define ARM64_INIT_STACK_TOP ((uintptr_t)0x03100000u)

#endif

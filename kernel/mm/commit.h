/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef MM_COMMIT_H
#define MM_COMMIT_H

#include "process.h"
#include <stdint.h>

int vm_commit_reserve(process_t *proc, uint32_t bytes);
void vm_commit_release(process_t *proc, uint32_t bytes);

#endif

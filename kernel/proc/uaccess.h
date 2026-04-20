/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef UACCESS_H
#define UACCESS_H

#include <stdint.h>
#include "process.h"

/*
 * uaccess_prepare: validate a user-space range in the target process.
 *
 * If write_access is non-zero, every page in the range must be writable from
 * user mode. Copy-on-write pages are broken eagerly so later kernel writes do
 * not mutate a shared frame behind the process's back.
 */
int uaccess_prepare(process_t *proc,
                    uint32_t user_addr,
                    uint32_t len,
                    int write_access);

/*
 * uaccess_copy_from_user / uaccess_copy_to_user: bounded copies between
 * kernel memory and the current process's user address space.
 *
 * Both helpers validate every page in the requested span and return -1 if the
 * user mapping is absent, supervisor-only, or not writable when writing.
 */
int uaccess_copy_from_user(process_t *proc,
                           void *dst,
                           uint32_t user_src,
                           uint32_t len);
int uaccess_copy_to_user(process_t *proc,
                         uint32_t user_dst,
                         const void *src,
                         uint32_t len);

/*
 * uaccess_copy_string_from_user: copy a NUL-terminated string from user space
 * into dst, including the trailing NUL. Returns 0 on success, -1 if the
 * string is unmapped or exceeds dstsz - 1 bytes.
 */
int uaccess_copy_string_from_user(process_t *proc,
                                  char *dst,
                                  uint32_t dstsz,
                                  uint32_t user_src);

#endif

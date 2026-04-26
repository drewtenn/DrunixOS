/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall.h - common syscall dispatch declarations and user-visible constants.
 *
 * Architecture-specific syscall numbers live under each arch proc directory.
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include "arch.h"
#include "syscall_numbers.h"
#include <stdint.h>

#define PROT_NONE 0x0u
#define PROT_READ 0x1u
#define PROT_WRITE 0x2u
#define PROT_EXEC 0x4u

#define MAP_PRIVATE 0x02u
#define MAP_ANONYMOUS 0x20u

#define CLONE_VM 0x00000100u
#define CLONE_FS 0x00000200u
#define CLONE_FILES 0x00000400u
#define CLONE_SIGHAND 0x00000800u
#define CLONE_VFORK 0x00004000u
#define CLONE_THREAD 0x00010000u
#define CLONE_SETTLS 0x00080000u
#define CLONE_PARENT_SETTID 0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID 0x01000000u

/*
 * Architecture entry code decodes user register state and forwards the syscall
 * number plus up to six arguments here.
 */
uint32_t syscall_handler(uint32_t sysno,
                         uint32_t arg0,
                         uint32_t arg1,
                         uint32_t arg2,
                         uint32_t arg3,
                         uint32_t arg4,
                         uint32_t arg5);
uint64_t syscall_dispatch_from_frame(arch_trap_frame_t *frame);

#ifdef KTEST_ENABLED
int syscall_stdout_would_fallback(void *desktop,
                                  uint32_t pid,
                                  const char *buf,
                                  uint32_t len);
#endif

#endif

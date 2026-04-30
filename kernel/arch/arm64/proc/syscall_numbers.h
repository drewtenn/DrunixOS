/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PROC_SYSCALL_NUMBERS_H
#define KERNEL_ARCH_ARM64_PROC_SYSCALL_NUMBERS_H

/*
 * ARM64 has its own Linux syscall table in proc/syscall.c. These SYS_* values
 * are the legacy Drunix compatibility numbers still accepted by that table.
 */
#define SYS_FORK 2
#define SYS_STAT 106
#define SYS_NANOSLEEP 162

#define SYS_DRUNIX_CLEAR 4000
#define SYS_DRUNIX_SCROLL_UP 4001
#define SYS_DRUNIX_SCROLL_DOWN 4002
#define SYS_DRUNIX_MODLOAD 4003
#define SYS_DRUNIX_TCGETATTR 4004
#define SYS_DRUNIX_TCSETATTR 4005
#define SYS_DRUNIX_TCSETPGRP 4006
#define SYS_DRUNIX_TCGETPGRP 4007
#define SYS_DRUNIX_GETDENTS_PATH 4008
#define SYS_DRUNIX_DISPLAY_CLAIM 4009

#endif

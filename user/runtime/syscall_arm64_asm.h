/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * syscall_arm64_asm.h — AArch64 `svc #0` trampolines.
 *
 * The AArch64 syscall convention is:
 *   x8       — syscall number
 *   x0..x5   — first six args (in/out)
 *   x0       — return value
 *
 * Both syscall_arm64.c (low-level helpers used by arm64init.elf) and
 * syscall_arm64_compat.c (Drunix's user libc on ARM64) need exactly
 * the same trampolines, so keep one definition here as `static inline`
 * and let each translation unit emit its own copy.  No external
 * symbols, no link-time interaction — safe for partial-libc links such
 * as arm64init.elf.
 */

#ifndef USER_LIB_SYSCALL_ARM64_ASM_H
#define USER_LIB_SYSCALL_ARM64_ASM_H

static inline long arm64_syscall0(long nr)
{
	register long x0 __asm__("x0");
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
	return x0;
}

static inline long arm64_syscall1(long nr, long a0)
{
	register long x0 __asm__("x0") = a0;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

static inline long arm64_syscall2(long nr, long a0, long a1)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
	return x0;
}

static inline long arm64_syscall3(long nr, long a0, long a1, long a2)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x8)
	                 : "memory");
	return x0;
}

static inline long arm64_syscall4(long nr, long a0, long a1, long a2, long a3)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
	                 : "memory");
	return x0;
}

static inline long
arm64_syscall5(long nr, long a0, long a1, long a2, long a3, long a4)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
	                 : "memory");
	return x0;
}

static inline long
arm64_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1),
	                   "r"(x2),
	                   "r"(x3),
	                   "r"(x4),
	                   "r"(x5),
	                   "r"(x8)
	                 : "memory");
	return x0;
}

#endif

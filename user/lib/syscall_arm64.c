/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "syscall_arm64.h"

long arm64_sys_write(int fd, const char *buf, unsigned long len)
{
	register long x0 __asm__("x0") = fd;
	register long x1 __asm__("x1") = (long)buf;
	register long x2 __asm__("x2") = (long)len;
	register long x8 __asm__("x8") = 64;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x8)
	                 : "memory");
	return x0;
}

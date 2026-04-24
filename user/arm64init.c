/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "syscall_arm64.h"

static void put(const char *s, unsigned long len)
{
	arm64_sys_write(1, s, len);
}

static int fail(void)
{
	put("ARM64 syscall: fail\n", 20);
	return 1;
}

int main(void)
{
	long fd;
	long n;
	long pid;
	long ppid;
	long tid;
	char buf[32];

	put("ARM64 init: entered\n", 20);

	pid = arm64_sys_getpid();
	ppid = arm64_sys_getppid();
	tid = arm64_sys_gettid();
	if (pid <= 0 || ppid < 0 || tid <= 0)
		return fail();
	put("ARM64 syscall: identity ok\n", 27);

	n = arm64_sys_getcwd(buf, sizeof(buf));
	if (n != 2 || buf[0] != '/' || buf[1] != '\0')
		return fail();
	put("ARM64 syscall: getcwd ok\n", 25);

	fd = arm64_sys_openat(-100, "/bin/arm64init", 0, 0);
	if (fd < 0)
		return fail();
	n = arm64_sys_read((int)fd, buf, 4);
	if (arm64_sys_close((int)fd) != 0)
		return fail();
	if (n != 4 || buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' ||
	    buf[3] != 'F')
		return fail();
	put("ARM64 syscall: open/read/close ok\n", 34);

	if (arm64_sys_openat(-100, "/missing-arm64-syscall", 0, 0) >= 0)
		return fail();
	put("ARM64 syscall: errno ok\n", 24);

	put("ARM64 init: pass\n", 17);
	return 0;
}

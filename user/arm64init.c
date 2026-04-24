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

static int contains_bytes(const char *buf, long len, const char *needle)
{
	long i;
	long j;
	long needle_len = 0;

	while (needle[needle_len])
		needle_len++;
	if (needle_len == 0 || len < needle_len)
		return 0;
	for (i = 0; i <= len - needle_len; i++) {
		for (j = 0; j < needle_len; j++) {
			if (buf[i + j] != needle[j])
				break;
		}
		if (j == needle_len)
			return 1;
	}
	return 0;
}

int main(void)
{
	long fd;
	long n;
	long pid;
	long ppid;
	long tid;
	char buf[32];
	char statbuf[256];
	char dirbuf[512];
	char utsbuf[390];
	char timebuf[16];

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
	if (n != 4 || buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' ||
	    buf[3] != 'F')
		return fail();
	if (arm64_sys_fstat((int)fd, statbuf) != 0)
		return fail();
	if (arm64_sys_close((int)fd) != 0)
		return fail();
	put("ARM64 syscall: open/read/close ok\n", 34);

	if (arm64_sys_newfstatat(-100, "/bin/arm64init", statbuf, 0) != 0)
		return fail();
	put("ARM64 syscall: metadata ok\n", 27);

	fd = arm64_sys_openat(-100, "/", 0200000, 0);
	if (fd < 0)
		return fail();
	n = arm64_sys_getdents64((int)fd, dirbuf, sizeof(dirbuf));
	if (arm64_sys_close((int)fd) != 0)
		return fail();
	if (n <= 0 || !contains_bytes(dirbuf, n, "bin"))
		return fail();
	put("ARM64 syscall: dirents ok\n", 26);

	if (arm64_sys_uname(utsbuf) != 0)
		return fail();
	if (arm64_sys_clock_gettime(0, timebuf) != 0)
		return fail();
	if (arm64_sys_gettimeofday(timebuf, 0) != 0)
		return fail();
	put("ARM64 syscall: time/info ok\n", 28);

	if (arm64_sys_openat(-100, "/missing-arm64-syscall", 0, 0) >= 0)
		return fail();
	put("ARM64 syscall: errno ok\n", 24);

	put("ARM64 init: pass\n", 17);
	return 0;
}

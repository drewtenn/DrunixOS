/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * dufstest.c - headless smoke test for the writable /dufs mount.
 */

#include "lib/syscall.h"

static int same_bytes(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

int main(void)
{
	static const char msg[] = "hello from /dufs\n";
	char buf[32];
	int fd;
	int n;

	fd = sys_create("/dufs/hello.txt");
	if (fd < 0) {
		sys_write("DUFSTEST create failed\n");
		return 1;
	}
	if (sys_fwrite(fd, msg, (int)sizeof(msg) - 1) != (int)sizeof(msg) - 1) {
		sys_write("DUFSTEST write failed\n");
		sys_close(fd);
		return 1;
	}
	sys_close(fd);

	fd = sys_open("/dufs/hello.txt");
	if (fd < 0) {
		sys_write("DUFSTEST reopen failed\n");
		return 1;
	}
	n = sys_read(fd, buf, (int)sizeof(msg) - 1);
	sys_close(fd);

	if (n != (int)sizeof(msg) - 1 ||
	    !same_bytes(buf, msg, (int)sizeof(msg) - 1)) {
		sys_write("DUFSTEST readback failed\n");
		return 1;
	}

	sys_write("DUFSTEST ok\n");
	return 0;
}

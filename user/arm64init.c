/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "syscall_arm64.h"

static void put(const char *s, unsigned long len)
{
	arm64_sys_write(1, s, len);
}

static int fail_msg(const char *msg, unsigned long len)
{
	put(msg, len);
	return 1;
}

static int fail(void)
{
	return fail_msg("ARM64 syscall: fail\n", 20);
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
	long dupfd;
	long dup3fd;
	char buf[32];
	char statbuf[256];
	char dirbuf[512];
	char utsbuf[390];
	char timebuf[16];
	unsigned char sigact[32];
	unsigned char oldsigact[32];
	unsigned char rtoldmask[8];
	unsigned int sigmask;
	int pipefds[2];
	int status;
	char pipec;
	long brk0;
	long map;

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

	brk0 = arm64_sys_brk(0);
	if (brk0 <= 0)
		return fail_msg("ARM64 syscall: fail brk query\n", 30);
	if (arm64_sys_brk((void *)(brk0 + 4096)) != brk0 + 4096)
		return fail_msg("ARM64 syscall: fail brk grow\n", 29);
	if (arm64_sys_brk((void *)brk0) != brk0)
		return fail_msg("ARM64 syscall: fail brk shrink\n", 31);
	map = arm64_sys_mmap(0, 4096, 3, 0x22, -1, 0);
	if (map < 0)
		return fail_msg("ARM64 syscall: fail mmap\n", 25);
	if (arm64_sys_mprotect((void *)map, 4096, 1) != 0)
		return fail_msg("ARM64 syscall: fail mprotect\n", 29);
	if (arm64_sys_munmap((void *)map, 4096) != 0)
		return fail_msg("ARM64 syscall: fail munmap\n", 27);
	put("ARM64 syscall: memory ok\n", 25);

	fd = arm64_sys_openat(-100, "/arm64.tmp", 0100 | 02 | 01000, 0644);
	if (fd < 0)
		return fail_msg("ARM64 syscall: fail create file\n", 32);
	if (arm64_sys_write((int)fd, "x", 1) != 1)
		return fail_msg("ARM64 syscall: fail write file\n", 31);
	if (arm64_sys_close((int)fd) != 0)
		return fail();
	if (arm64_sys_unlinkat(-100, "/arm64.tmp", 0) != 0)
		return fail_msg("ARM64 syscall: fail unlink file\n", 32);
	if (arm64_sys_mkdirat(-100, "/arm64.dir", 0755) != 0)
		return fail_msg("ARM64 syscall: fail mkdir\n", 26);
	if (arm64_sys_unlinkat(-100, "/arm64.dir", 0x200) != 0)
		return fail_msg("ARM64 syscall: fail rmdir\n", 26);
	put("ARM64 syscall: mutation ok\n", 27);

	if (arm64_sys_faccessat(-100, "/bin/arm64init", 0) != 0)
		return fail_msg("ARM64 syscall: fail faccessat\n", 30);
	if (arm64_sys_chdir("/bin") != 0)
		return fail_msg("ARM64 syscall: fail chdir\n", 26);
	n = arm64_sys_getcwd(buf, sizeof(buf));
	if (n != 5 || buf[0] != '/' || buf[1] != 'b' || buf[2] != 'i' ||
	    buf[3] != 'n' || buf[4] != '\0')
		return fail_msg("ARM64 syscall: fail cwd bin\n", 28);
	fd = arm64_sys_openat(-100, "/bin/arm64init", 0, 0);
	if (fd < 0)
		return fail();
	if (arm64_sys_lseek((int)fd, 0, 0) != 0)
		return fail_msg("ARM64 syscall: fail lseek\n", 26);
	dupfd = arm64_sys_dup((int)fd);
	if (dupfd < 0)
		return fail_msg("ARM64 syscall: fail dup\n", 24);
	if (arm64_sys_fcntl((int)dupfd, 3, 0) < 0)
		return fail_msg("ARM64 syscall: fail fcntl\n", 26);
	dup3fd = arm64_sys_dup3((int)fd, 10, 02000000);
	if (dup3fd != 10 || arm64_sys_fcntl(10, 1, 0) != 1)
		return fail_msg("ARM64 syscall: fail dup3\n", 25);
	if (arm64_sys_ioctl((int)dupfd, 0x5413, buf) == -38)
		return fail();
	if (arm64_sys_close((int)dup3fd) != 0 ||
	    arm64_sys_close((int)dupfd) != 0 || arm64_sys_close((int)fd) != 0)
		return fail();
	if (arm64_sys_readlinkat(-100, "/bin/arm64init", buf, sizeof(buf)) == -38)
		return fail_msg("ARM64 syscall: fail readlinkat\n", 31);
	if (arm64_sys_pipe2(pipefds, 0) != 0)
		return fail_msg("ARM64 syscall: fail pipe2\n", 26);
	if (arm64_sys_write(pipefds[1], "q", 1) != 1 ||
	    arm64_sys_read(pipefds[0], &pipec, 1) != 1 || pipec != 'q')
		return fail_msg("ARM64 syscall: fail pipe rw\n", 28);
	if (arm64_sys_close(pipefds[0]) != 0 || arm64_sys_close(pipefds[1]) != 0)
		return fail();
	put("ARM64 syscall: fd/path ok\n", 26);

	if (arm64_sys_kill((int)pid, 0) != 0)
		return fail_msg("ARM64 syscall: fail kill\n", 25);
	sigmask = 1u << 15;
	if (arm64_sys_rt_sigprocmask(2, &sigmask, rtoldmask, sizeof(rtoldmask)) !=
	    0)
		return fail_msg("ARM64 syscall: fail rt sigmask\n", 31);
	sigmask = 0;
	if (arm64_sys_rt_sigprocmask(2, &sigmask, rtoldmask, sizeof(rtoldmask)) !=
	    0)
		return fail_msg("ARM64 syscall: fail rt sigmask\n", 31);
	for (n = 0; n < (long)sizeof(sigact); n++) {
		sigact[n] = 0;
		oldsigact[n] = 0;
	}
	sigact[0] = 1;
	if (arm64_sys_rt_sigaction(15, sigact, oldsigact, 8) != 0 ||
	    arm64_sys_rt_sigaction(15, 0, oldsigact, 8) != 0 ||
	    oldsigact[0] != 1)
		return fail_msg("ARM64 syscall: fail rt sigaction\n", 33);
	put("ARM64 syscall: signal ok\n", 25);

	if (arm64_sys_clone(0x00000800u | 17u, 0, 0, 0, 0) != -22)
		return fail_msg("ARM64 syscall: fail clone validation\n", 37);
	status = 0;
	if (arm64_sys_wait4(-1, &status, 1, 0) != -1)
		return fail_msg("ARM64 syscall: fail wait4 empty\n", 32);
	if (arm64_sys_execve("/missing-arm64-exec", 0, 0) != -1)
		return fail_msg("ARM64 syscall: fail exec missing\n", 33);
	put("ARM64 syscall: process ok\n", 26);

	if (arm64_sys_openat(-100, "/missing-arm64-syscall", 0, 0) >= 0)
		return fail();
	put("ARM64 syscall: errno ok\n", 24);

	put("ARM64 init: pass\n", 17);
	return 0;
}

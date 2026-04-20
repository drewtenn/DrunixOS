/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * stdlib.c — atoi/abs/exit/abort.
 *
 * exit() is the libc bridge to SYS_EXIT.  abort() routes through SYS_KILL
 * with SIGABRT, which the kernel delivers to the caller and which, under
 * the default disposition installed at process creation, terminates the
 * process.  Neither function returns; the trailing infinite loops tell
 * the compiler that and suppress "noreturn function returns" diagnostics.
 */

#include "stdlib.h"
#include "syscall.h"
#include "string.h"

int atoi(const char *s)
{
	int sign = 1;
	int n = 0;

	/* Skip leading whitespace (matches glibc strtol-style behaviour). */
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' ||
	       *s == '\f')
		s++;

	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}

	while (*s >= '0' && *s <= '9') {
		n = n * 10 + (*s - '0');
		s++;
	}
	return sign * n;
}

int abs(int x)
{
	return x < 0 ? -x : x;
}

char *getenv(const char *name)
{
	size_t len;

	if (!name || !*name || strchr(name, '='))
		return 0;

	len = strlen(name);
	if (!environ)
		return 0;

	for (int i = 0; environ[i]; i++) {
		if (strncmp(environ[i], name, len) == 0 && environ[i][len] == '=')
			return environ[i] + len + 1;
	}
	return 0;
}

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
	while (size--) {
		unsigned char t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

static void qsort_range(unsigned char *base,
                        int left,
                        int right,
                        size_t size,
                        int (*compar)(const void *, const void *))
{
	int i = left;
	int j = right;
	unsigned char *pivot = base + ((left + (right - left) / 2) * (int)size);

	while (i <= j) {
		while (compar(base + i * (int)size, pivot) < 0)
			i++;
		while (compar(base + j * (int)size, pivot) > 0)
			j--;
		if (i <= j) {
			if (i != j) {
				swap_bytes(base + i * (int)size, base + j * (int)size, size);
				if (pivot == base + i * (int)size)
					pivot = base + j * (int)size;
				else if (pivot == base + j * (int)size)
					pivot = base + i * (int)size;
			}
			i++;
			j--;
		}
	}

	if (left < j)
		qsort_range(base, left, j, size, compar);
	if (i < right)
		qsort_range(base, i, right, size, compar);
}

void qsort(void *base,
           size_t nmemb,
           size_t size,
           int (*compar)(const void *, const void *))
{
	if (!base || !compar || nmemb < 2 || size == 0)
		return;
	qsort_range((unsigned char *)base, 0, (int)nmemb - 1, size, compar);
}

void exit(int status)
{
	sys_exit(status);
	for (;;) {
	} /* unreachable — sys_exit never returns */
}

void abort(void)
{
	sys_kill(sys_getpid(), SIGABRT);
	/*
     * SIGABRT under the default disposition terminates the process, so the
     * kill() syscall above does not return.  Belt-and-braces: if a user
     * process has installed a handler that catches SIGABRT and returns,
     * fall through to sys_exit(128 + SIGABRT) so abort() still never
     * returns.  134 is the conventional "aborted" exit code on POSIX.
     */
	sys_exit(128 + SIGABRT);
	for (;;) {
	}
}

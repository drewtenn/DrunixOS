/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_info.c - Linux i386 system and resource information syscalls.
 *
 * Handles informational and resource-query compatibility calls such as uname,
 * sysinfo, getrusage, prlimit64, priority stubs, and CPU affinity reporting.
 */

#include "syscall_internal.h"
#include "clock.h"
#include "kstring.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include <stdint.h>

#define LINUX_EINVAL 22
#define LINUX_RLIMIT_NLIMITS 16u
#define LINUX_RLIMIT_STACK 3u

static void info_put_u32(uint8_t *buf, uint32_t off, uint32_t value)
{
	buf[off + 0] = (uint8_t)(value & 0xFFu);
	buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
	buf[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
	buf[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void info_put_u16(uint8_t *buf, uint32_t off, uint32_t value)
{
	buf[off + 0] = (uint8_t)(value & 0xFFu);
	buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void info_put_u64(uint8_t *buf, uint32_t off, uint64_t value)
{
	info_put_u32(buf, off, (uint32_t)value);
	info_put_u32(buf, off + 4u, (uint32_t)(value >> 32));
}

static void
info_copy_field(char *dst, uint32_t off, uint32_t len, const char *src)
{
	uint32_t i = 0;

	if (!dst || len == 0)
		return;
	while (src && src[i] && i + 1u < len) {
		dst[off + i] = src[i];
		i++;
	}
	dst[off + i] = '\0';
}

static uint32_t syscall_sysinfo(uint32_t user_info)
{
	process_t *cur = sched_current();
	uint8_t info[64];
	uint32_t pids[MAX_PROCS];
	int n;

	if (!cur || user_info == 0)
		return (uint32_t)-1;
	n = sched_snapshot_pids(pids, MAX_PROCS, 1);
	k_memset(info, 0, sizeof(info));
	info_put_u32(info, 0u, clock_unix_time());
	info_put_u32(info, 16u, 16u * 1024u * 1024u);
	info_put_u32(info, 20u, 8u * 1024u * 1024u);
	info_put_u16(info, 40u, (uint32_t)(n < 0 ? 0 : n));
	info_put_u32(info, 52u, 1u);
	return uaccess_copy_to_user(cur, user_info, info, sizeof(info)) == 0
	           ? 0
	           : (uint32_t)-1;
}

static uint32_t syscall_uname(uint32_t user_uts)
{
	process_t *cur = sched_current();
	char uts[390];

	if (!cur || user_uts == 0)
		return (uint32_t)-1;
	k_memset(uts, 0, sizeof(uts));
	info_copy_field(uts, 0u, 65u, "Drunix");
	info_copy_field(uts, 65u, 65u, "drunix");
	info_copy_field(uts, 130u, 65u, "0.1");
	info_copy_field(uts, 195u, 65u, "Drunix Linux i386 ABI");
	info_copy_field(uts, 260u, 65u, "i486");
	info_copy_field(uts, 325u, 65u, "drunix.local");
	if (uaccess_copy_to_user(cur, user_uts, uts, sizeof(uts)) != 0)
		return (uint32_t)-1;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_getrusage(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();
	uint8_t zero[72];

	if (!cur || ecx == 0)
		return (uint32_t)-1;
	if (ebx != 0 && ebx != 0xFFFFFFFFu)
		return (uint32_t)-LINUX_EINVAL;
	k_memset(zero, 0, sizeof(zero));
	return uaccess_copy_to_user(cur, ecx, zero, sizeof(zero)) == 0
	           ? 0
	           : (uint32_t)-1;
}

uint32_t SYSCALL_NOINLINE syscall_case_getpriority(void)
{

	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_setpriority(void)
{

	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_sysinfo(uint32_t ebx)
{

	return syscall_sysinfo(ebx);
}

uint32_t SYSCALL_NOINLINE syscall_case_prlimit64(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t edx,
                                                 uint32_t esi)
{
	process_t *cur = sched_current();
	uint8_t rlim[16];
	uint64_t value = ~(uint64_t)0;

	/*
	 * Linux i386 prlimit64(pid, resource, new_limit, old_limit). Static musl
	 * probes process limits; Drunix reports stable limits and rejects attempts
	 * to change them for now.
	 */
	if (!cur || ecx >= LINUX_RLIMIT_NLIMITS)
		return (uint32_t)-1;
	if (ebx != 0 && ebx != sched_current_tgid() && ebx != sched_current_tid())
		return (uint32_t)-1;
	if (edx != 0)
		return (uint32_t)-1;
	if (esi == 0)
		return 0;

	if (ecx == LINUX_RLIMIT_STACK)
		value = (uint64_t)USER_STACK_MAX_PAGES * PAGE_SIZE;
	info_put_u64(rlim, 0u, value);
	info_put_u64(rlim, 8u, value);
	return uaccess_copy_to_user(cur, esi, rlim, sizeof(rlim)) == 0
	           ? 0
	           : (uint32_t)-1;
}

uint32_t SYSCALL_NOINLINE syscall_case_sched_getaffinity(uint32_t ecx,
                                                         uint32_t edx)
{
	process_t *cur = sched_current();
	uint8_t mask[4];

	if (!cur || edx == 0 || ecx < sizeof(mask))
		return (uint32_t)-LINUX_EINVAL;
	k_memset(mask, 0, sizeof(mask));
	mask[0] = 1u;
	if (uaccess_copy_to_user(cur, edx, mask, sizeof(mask)) != 0)
		return (uint32_t)-1;
	return sizeof(mask);
}

uint32_t SYSCALL_NOINLINE syscall_case_uname(uint32_t ebx)
{

	/*
	 * ebx = struct utsname *.
	 * Linux i386 old_utsname fields are 65-byte NUL-terminated strings.
	 */
	return syscall_uname(ebx);
}

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * time.c - Linux i386 time-related syscall implementations.
 *
 * Handles sleep and wall/monotonic clock queries. Keep time ABI packing and
 * clock validation here; scheduler and clock subsystems remain the source of
 * truth for elapsed time.
 */

#include "syscall_internal.h"
#include "arch.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include <stdint.h>

#define LINUX_EFAULT 14
#define LINUX_EINVAL 22

static uint32_t syscall_nanosleep(uint32_t user_req, uint32_t user_rem)
{
	process_t *cur = sched_current();
	uint32_t req[2];
	uint32_t start;
	uint32_t sec_ticks;
	uint32_t tick_nsec;
	uint32_t nsec_ticks;
	uint32_t delta_ticks;
	uint32_t deadline;
	uint32_t now;

	if (!cur)
		return (uint32_t)-1;
	if (user_req == 0)
		return (uint32_t)-LINUX_EFAULT;
	if (uaccess_copy_from_user(cur, req, user_req, sizeof(req)) != 0)
		return (uint32_t)-LINUX_EFAULT;
	if (req[1] >= 1000000000u)
		return (uint32_t)-LINUX_EINVAL;
	if (req[0] == 0 && req[1] == 0)
		return 0;

	start = sched_ticks();
	sec_ticks =
	    (req[0] > (0xFFFFFFFFu / SCHED_HZ)) ? 0xFFFFFFFFu : req[0] * SCHED_HZ;
	tick_nsec = 1000000000u / SCHED_HZ;
	nsec_ticks = (req[1] == 0) ? 0 : (req[1] + tick_nsec - 1u) / tick_nsec;
	delta_ticks = (sec_ticks > 0xFFFFFFFFu - nsec_ticks)
	                  ? 0xFFFFFFFFu
	                  : sec_ticks + nsec_ticks;
	deadline = start + delta_ticks;

	sched_block_until(deadline);

	now = sched_ticks();
	if ((int32_t)(deadline - now) > 0) {
		uint32_t remaining_ticks = deadline - now;
		if (user_rem != 0) {
			uint32_t rem[2];
			rem[0] = remaining_ticks / SCHED_HZ;
			rem[1] = (remaining_ticks % SCHED_HZ) * tick_nsec;
			if (uaccess_copy_to_user(cur, user_rem, rem, sizeof(rem)) != 0)
				return (uint32_t)-1;
		}
		return (uint32_t)-1;
	}
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_nanosleep(uint32_t ebx, uint32_t ecx)
{

	/*
	 * ebx = const struct timespec *req, ecx = struct timespec *rem.
	 *
	 * Blocks the caller until the deadline expires or a signal wakes it.
	 * Returns 0 on full sleep, or -1 after copying remaining time when
	 * interrupted by a signal.
	 */
	return syscall_nanosleep(ebx, ecx);
}

uint32_t SYSCALL_NOINLINE syscall_case_clock_gettime(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();
	uint32_t ts[2];
	uint32_t ticks;

	/*
	 * ebx = clock id (0 = CLOCK_REALTIME, 1 = CLOCK_MONOTONIC).
	 * ecx = pointer to struct timespec in user space:
	 *       long tv_sec; long tv_nsec;
	 *
	 * Returns 0 on success, -EINVAL for an unsupported clock, or -1 for a
	 * bad pointer.
	 */
	if (!cur || ecx == 0)
		return (uint32_t)-1;
	if (ebx == 0) {
		ts[0] = arch_time_unix_seconds();
		ts[1] = 0;
	} else if (ebx == 1) {
		ticks = arch_time_uptime_ticks();
		ts[0] = ticks / SCHED_HZ;
		ts[1] = (ticks % SCHED_HZ) * (1000000000u / SCHED_HZ);
	} else {
		return (uint32_t)-LINUX_EINVAL;
	}
	if (uaccess_copy_to_user(cur, ecx, ts, sizeof(ts)) != 0)
		return (uint32_t)-1;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_clock_gettime64(uint32_t ebx,
                                                       uint32_t ecx)
{
	process_t *cur = sched_current();
	uint32_t ts64[4];
	uint32_t ticks;

	/*
	 * ebx = clock id, ecx = struct __kernel_timespec64 *.
	 * Linux time64 ABI uses 64-bit seconds and nanoseconds on i386.
	 */
	if (!cur || ecx == 0)
		return (uint32_t)-1;
	if (ebx == 0) {
		ts64[0] = arch_time_unix_seconds();
		ts64[1] = 0;
		ts64[2] = 0;
		ts64[3] = 0;
	} else if (ebx == 1) {
		ticks = arch_time_uptime_ticks();
		ts64[0] = ticks / SCHED_HZ;
		ts64[1] = 0;
		ts64[2] = (ticks % SCHED_HZ) * (1000000000u / SCHED_HZ);
		ts64[3] = 0;
	} else {
		return (uint32_t)-LINUX_EINVAL;
	}
	if (uaccess_copy_to_user(cur, ecx, ts64, sizeof(ts64)) != 0)
		return (uint32_t)-1;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_gettimeofday(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();
	uint32_t tv[2];
	uint32_t tz[2] = {0, 0};

	/*
	 * ebx = struct timeval32 *, ecx = struct timezone *.
	 * The timezone argument is obsolete; Linux still zeros it when given.
	 */
	if (!cur)
		return (uint32_t)-1;
	if (ebx != 0) {
		tv[0] = arch_time_unix_seconds();
		tv[1] = 0;
		if (uaccess_copy_to_user(cur, ebx, tv, sizeof(tv)) != 0)
			return (uint32_t)-1;
	}
	if (ecx != 0 && uaccess_copy_to_user(cur, ecx, tz, sizeof(tz)) != 0)
		return (uint32_t)-1;
	return 0;
}

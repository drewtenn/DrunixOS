/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TIME_H
#define TIME_H

#include <stddef.h>

typedef long time_t;

struct timespec {
	time_t tv_sec;
	long tv_nsec;
};

struct tm {
	int tm_sec;   /* seconds after the minute, 0-60 */
	int tm_min;   /* minutes after the hour, 0-59 */
	int tm_hour;  /* hours since midnight, 0-23 */
	int tm_mday;  /* day of the month, 1-31 */
	int tm_mon;   /* months since January, 0-11 */
	int tm_year;  /* years since 1900 */
	int tm_wday;  /* days since Sunday, 0-6 */
	int tm_yday;  /* days since January 1, 0-365 */
	int tm_isdst; /* daylight saving flag; always 0 for UTC */
};

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_REALTIME 0

int clock_gettime(int clock_id, struct timespec *ts);
time_t time(time_t *tloc);

struct tm *gmtime(const time_t *timer);
struct tm *gmtime_r(const time_t *timer, struct tm *result);
struct tm *localtime(const time_t *timer);
struct tm *localtime_r(const time_t *timer, struct tm *result);

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif

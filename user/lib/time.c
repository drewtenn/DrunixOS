/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * time.c — user-space time conversion helpers and syscall wrappers.
 */

#include "time.h"
#include "syscall.h"
#include "string.h"

static const char *const wday_short[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *const wday_long[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static const char *const month_short[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const month_long[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int is_leap_year(int year)
{
    return ((year % 4) == 0 && (year % 100) != 0) ||
           ((year % 400) == 0);
}

static int month_len(int year, int month)
{
    static const int days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 1 && is_leap_year(year))
        return 29;
    return days[month];
}

int clock_gettime(int clock_id, struct timespec *ts)
{
    return sys_clock_gettime(clock_id, (sys_timespec_t *)ts);
}

time_t time(time_t *tloc)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return (time_t)-1;
    if (tloc)
        *tloc = ts.tv_sec;
    return ts.tv_sec;
}

struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
    if (!timer || !result)
        return 0;

    time_t seconds = *timer;
    if (seconds < 0)
        return 0;

    unsigned int days = (unsigned int)(seconds / 86400);
    unsigned int rem = (unsigned int)(seconds % 86400);

    result->tm_hour = (int)(rem / 3600);
    rem %= 3600;
    result->tm_min = (int)(rem / 60);
    result->tm_sec = (int)(rem % 60);

    result->tm_wday = (int)((days + 4) % 7); /* 1970-01-01 was Thursday. */

    int year = 1970;
    while (1) {
        int ydays = is_leap_year(year) ? 366 : 365;
        if (days < (unsigned int)ydays)
            break;
        days -= (unsigned int)ydays;
        year++;
    }

    result->tm_year = year - 1900;
    result->tm_yday = (int)days;

    int month = 0;
    while (month < 12) {
        int mdays = month_len(year, month);
        if (days < (unsigned int)mdays)
            break;
        days -= (unsigned int)mdays;
        month++;
    }

    result->tm_mon = month;
    result->tm_mday = (int)days + 1;
    result->tm_isdst = 0;
    return result;
}

struct tm *gmtime(const time_t *timer)
{
    static struct tm result;
    return gmtime_r(timer, &result);
}

struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    return gmtime_r(timer, result);
}

struct tm *localtime(const time_t *timer)
{
    static struct tm result;
    return localtime_r(timer, &result);
}

static int append_char(char *s, size_t max, size_t *pos, char c)
{
    if (*pos + 1 >= max)
        return -1;
    s[(*pos)++] = c;
    return 0;
}

static int append_str(char *s, size_t max, size_t *pos, const char *text)
{
    while (*text) {
        if (append_char(s, max, pos, *text++) != 0)
            return -1;
    }
    return 0;
}

static int append_uint(char *s, size_t max, size_t *pos,
                       unsigned int value, int width, char pad)
{
    char tmp[16];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value && n < (int)sizeof(tmp));

    while (n < width) {
        if (append_char(s, max, pos, pad) != 0)
            return -1;
        width--;
    }

    while (n > 0) {
        if (append_char(s, max, pos, tmp[--n]) != 0)
            return -1;
    }
    return 0;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
    size_t pos = 0;

    if (!s || max == 0 || !format || !tm)
        return 0;

    while (*format) {
        if (*format != '%') {
            if (append_char(s, max, &pos, *format++) != 0)
                return 0;
            continue;
        }

        format++;
        switch (*format) {
        case '\0':
            if (append_char(s, max, &pos, '%') != 0) return 0;
            break;
        case '%':
            if (append_char(s, max, &pos, '%') != 0) return 0;
            break;
        case 'a':
            if (append_str(s, max, &pos, wday_short[tm->tm_wday]) != 0) return 0;
            break;
        case 'A':
            if (append_str(s, max, &pos, wday_long[tm->tm_wday]) != 0) return 0;
            break;
        case 'b':
        case 'h':
            if (append_str(s, max, &pos, month_short[tm->tm_mon]) != 0) return 0;
            break;
        case 'B':
            if (append_str(s, max, &pos, month_long[tm->tm_mon]) != 0) return 0;
            break;
        case 'c':
            if (strftime(s + pos, max - pos, "%a %b %e %H:%M:%S UTC %Y", tm) == 0)
                return 0;
            pos += strlen(s + pos);
            break;
        case 'd':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_mday, 2, '0') != 0) return 0;
            break;
        case 'e':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_mday, 2, ' ') != 0) return 0;
            break;
        case 'H':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_hour, 2, '0') != 0) return 0;
            break;
        case 'j':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_yday + 1u, 3, '0') != 0) return 0;
            break;
        case 'm':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_mon + 1u, 2, '0') != 0) return 0;
            break;
        case 'M':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_min, 2, '0') != 0) return 0;
            break;
        case 'S':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_sec, 2, '0') != 0) return 0;
            break;
        case 'w':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_wday, 1, '0') != 0) return 0;
            break;
        case 'Y':
            if (append_uint(s, max, &pos, (unsigned int)tm->tm_year + 1900u, 4, '0') != 0) return 0;
            break;
        case 'Z':
            if (append_str(s, max, &pos, "UTC") != 0) return 0;
            break;
        default:
            if (append_char(s, max, &pos, '%') != 0) return 0;
            if (append_char(s, max, &pos, *format) != 0) return 0;
            break;
        }

        if (*format)
            format++;
    }

    s[pos] = '\0';
    return pos;
}

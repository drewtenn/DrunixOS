/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * clock.c — wall-clock and monotonic time helpers backed by the RTC and PIT tick counter.
 */

#include "clock.h"
#include "io.h"
#include "sched.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define CMOS_REG_SECONDS 0x00
#define CMOS_REG_MINUTES 0x02
#define CMOS_REG_HOURS 0x04
#define CMOS_REG_DAY 0x07
#define CMOS_REG_MONTH 0x08
#define CMOS_REG_YEAR 0x09
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B

static uint32_t g_unix_time;
static uint32_t g_subsecond_ticks;
static uint32_t g_uptime_ticks;

typedef struct {
	uint32_t year;
	uint32_t month;
	uint32_t day;
	uint32_t hour;
	uint32_t minute;
	uint32_t second;
} rtc_time_t;

static uint8_t cmos_read(uint8_t reg)
{
	port_byte_out(CMOS_ADDR, reg);
	return port_byte_in(CMOS_DATA);
}

static int rtc_update_in_progress(void)
{
	return (cmos_read(CMOS_REG_STATUS_A) & 0x80) != 0;
}

static uint8_t bcd_to_binary(uint8_t value)
{
	return (uint8_t)((value & 0x0Fu) + ((value >> 4) * 10u));
}

static int rtc_read_stable(rtc_time_t *out, uint8_t *status_b_out)
{
	rtc_time_t a;
	rtc_time_t b;
	uint8_t status_b;

	for (uint32_t tries = 0; tries < 100000; tries++) {
		if (rtc_update_in_progress())
			continue;

		a.second = cmos_read(CMOS_REG_SECONDS);
		a.minute = cmos_read(CMOS_REG_MINUTES);
		a.hour = cmos_read(CMOS_REG_HOURS);
		a.day = cmos_read(CMOS_REG_DAY);
		a.month = cmos_read(CMOS_REG_MONTH);
		a.year = cmos_read(CMOS_REG_YEAR);
		status_b = cmos_read(CMOS_REG_STATUS_B);

		if (rtc_update_in_progress())
			continue;

		b.second = cmos_read(CMOS_REG_SECONDS);
		b.minute = cmos_read(CMOS_REG_MINUTES);
		b.hour = cmos_read(CMOS_REG_HOURS);
		b.day = cmos_read(CMOS_REG_DAY);
		b.month = cmos_read(CMOS_REG_MONTH);
		b.year = cmos_read(CMOS_REG_YEAR);

		if (a.second == b.second && a.minute == b.minute && a.hour == b.hour &&
		    a.day == b.day && a.month == b.month && a.year == b.year) {
			*out = a;
			*status_b_out = status_b;
			return 0;
		}
	}

	return -1;
}

static int is_leap_year(uint32_t year)
{
	return ((year % 4u) == 0u && (year % 100u) != 0u) || ((year % 400u) == 0u);
}

static uint32_t days_before_month(uint32_t year, uint32_t month)
{
	static const uint16_t normal[] = {
	    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	uint32_t days = normal[month - 1u];
	if (month > 2u && is_leap_year(year))
		days++;
	return days;
}

static uint32_t rtc_to_unix(const rtc_time_t *t)
{
	uint32_t days = 0;
	for (uint32_t year = 1970; year < t->year; year++)
		days += is_leap_year(year) ? 366u : 365u;

	days += days_before_month(t->year, t->month);
	days += t->day - 1u;

	return days * 86400u + t->hour * 3600u + t->minute * 60u + t->second;
}

void clock_init(void)
{
	rtc_time_t rtc;
	uint8_t status_b;

	g_unix_time = 0;
	g_subsecond_ticks = 0;
	g_uptime_ticks = 0;

	if (rtc_read_stable(&rtc, &status_b) != 0)
		return;

	if ((status_b & 0x04u) == 0) {
		rtc.second = bcd_to_binary((uint8_t)rtc.second);
		rtc.minute = bcd_to_binary((uint8_t)rtc.minute);
		rtc.hour = (uint32_t)bcd_to_binary((uint8_t)(rtc.hour & 0x7Fu)) |
		           (rtc.hour & 0x80u);
		rtc.day = bcd_to_binary((uint8_t)rtc.day);
		rtc.month = bcd_to_binary((uint8_t)rtc.month);
		rtc.year = bcd_to_binary((uint8_t)rtc.year);
	}

	if ((status_b & 0x02u) == 0) {
		uint32_t pm = rtc.hour & 0x80u;
		rtc.hour &= 0x7Fu;
		rtc.hour %= 12u;
		if (pm)
			rtc.hour += 12u;
	} else {
		rtc.hour &= 0x7Fu;
	}

	rtc.year += (rtc.year < 70u) ? 2000u : 1900u;

	if (rtc.year < 1970u || rtc.month < 1u || rtc.month > 12u || rtc.day < 1u ||
	    rtc.day > 31u || rtc.hour > 23u || rtc.minute > 59u || rtc.second > 59u)
		return;

	g_unix_time = rtc_to_unix(&rtc);
}

void clock_tick(void)
{
	g_uptime_ticks++;
	g_subsecond_ticks++;
	if (g_subsecond_ticks >= SCHED_HZ) {
		g_subsecond_ticks = 0;
		g_unix_time++;
	}
}

uint32_t clock_unix_time(void)
{
	return g_unix_time;
}

uint32_t clock_uptime_ticks(void)
{
	return g_uptime_ticks;
}

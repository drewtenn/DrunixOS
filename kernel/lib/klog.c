/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * klog.c — kernel logging helpers with console mirroring and a retained ring buffer.
 */

#include "klog.h"
#include "arch.h"
#include "sched.h"
#include "kprintf.h"
#include "kstring.h"

#define KLOG_RING_CAP 96u
#define KLOG_TAG_CAP 16u
#define KLOG_MSG_CAP 96u

typedef struct {
	uint32_t ticks;
	uint8_t level;
	char tag[KLOG_TAG_CAP];
	char msg[KLOG_MSG_CAP];
} klog_record_t;

static klog_record_t g_klog_ring[KLOG_RING_CAP];
static uint32_t g_klog_head;
static uint32_t g_klog_count;

static void klog_debugcon_puts(const char *s)
{
	if (!s)
		return;
	arch_debug_write(s, k_strlen(s));
}

static void klog_puts(const char *s)
{
	if (!s)
		return;
	arch_console_write(s, k_strlen(s));
	klog_debugcon_puts(s);
}

static void klog_puts_silent(const char *s)
{
	klog_debugcon_puts(s);
}

static const char *klog_level_name(klog_level_t level)
{
	switch (level) {
	case KLOG_LEVEL_DEBUG:
		return "DEBUG";
	case KLOG_LEVEL_INFO:
		return "INFO";
	case KLOG_LEVEL_WARN:
		return "WARN";
	case KLOG_LEVEL_ERROR:
		return "ERROR";
	default:
		return "INFO";
	}
}

static void klog_store_record(const klog_record_t *rec)
{
	klog_record_t *slot;

	if (!rec)
		return;

	slot = &g_klog_ring[g_klog_head];
	*slot = *rec;

	g_klog_head = (g_klog_head + 1u) % KLOG_RING_CAP;
	if (g_klog_count < KLOG_RING_CAP)
		g_klog_count++;
}

static int klog_format_line(char *buf,
                            uint32_t cap,
                            uint32_t ticks,
                            klog_level_t level,
                            const char *tag,
                            const char *msg)
{
	uint32_t secs = ticks / SCHED_HZ;
	uint32_t ms = ((ticks % SCHED_HZ) * 1000u) / SCHED_HZ;

	return k_snprintf(buf,
	                  cap,
	                  "[%u.%03u] %s [%s] %s\n",
	                  secs,
	                  ms,
	                  klog_level_name(level),
	                  tag,
	                  msg);
}

static uint32_t klog_snapshot_start(void)
{
	if (g_klog_count < KLOG_RING_CAP)
		return 0;
	return g_klog_head;
}

static const klog_record_t *klog_record_at(uint32_t logical_index)
{
	uint32_t start = klog_snapshot_start();
	uint32_t slot = (start + logical_index) % KLOG_RING_CAP;

	return &g_klog_ring[slot];
}

static void klog_emit_record(const klog_record_t *rec)
{
	char line[160];

	if (!rec)
		return;

	klog_format_line(line,
	                 sizeof(line),
	                 rec->ticks,
	                 (klog_level_t)rec->level,
	                 rec->tag,
	                 rec->msg);
	klog_puts(line);
}

static void klog_emit_record_silent(const klog_record_t *rec)
{
	char line[160];

	if (!rec)
		return;

	klog_format_line(line,
	                 sizeof(line),
	                 rec->ticks,
	                 (klog_level_t)rec->level,
	                 rec->tag,
	                 rec->msg);
	klog_puts_silent(line);
}

static void klog_render_into(char *buf, uint32_t cap, uint32_t *size_out)
{
	uint32_t len = 0;

	for (uint32_t i = 0; i < g_klog_count; i++) {
		const klog_record_t *rec = klog_record_at(i);
		int n = klog_format_line(buf ? buf + len : 0,
		                         (cap > len) ? cap - len : 0,
		                         rec->ticks,
		                         (klog_level_t)rec->level,
		                         rec->tag,
		                         rec->msg);
		if (n > 0)
			len += (uint32_t)n;
	}
	if (size_out)
		*size_out = len;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void klog_log(klog_level_t level, const char *tag, const char *msg)
{
	klog_record_t rec;

	k_memset(&rec, 0, sizeof(rec));
	rec.ticks = arch_time_uptime_ticks();
	rec.level = (uint8_t)level;
	k_strncpy(rec.tag, tag ? tag : "KLOG", KLOG_TAG_CAP - 1u);
	rec.tag[KLOG_TAG_CAP - 1u] = '\0';
	k_strncpy(rec.msg, msg ? msg : "(null)", KLOG_MSG_CAP - 1u);
	rec.msg[KLOG_MSG_CAP - 1u] = '\0';

	klog_store_record(&rec);
	klog_emit_record(&rec);
}

void klog(const char *tag, const char *msg)
{
	klog_log(KLOG_LEVEL_INFO, tag, msg);
}

void klog_uint(const char *tag, const char *msg, uint32_t val)
{
	char line[KLOG_MSG_CAP];

	k_snprintf(line, sizeof(line), "%s: %u", msg ? msg : "(null)", val);
	klog_log(KLOG_LEVEL_INFO, tag, line);
}

void klog_hex(const char *tag, const char *msg, uint32_t val)
{
	char line[KLOG_MSG_CAP];

	k_snprintf(line, sizeof(line), "%s: 0x%08X", msg ? msg : "(null)", val);
	klog_log(KLOG_LEVEL_INFO, tag, line);
}

int klog_snapshot(char *buf, uint32_t cap, uint32_t *size_out)
{
	if (buf && cap > 0)
		buf[0] = '\0';
	klog_render_into(buf, cap, size_out);
	return 0;
}

void klog_log_silent(klog_level_t level, const char *tag, const char *msg)
{
	klog_record_t rec;

	k_memset(&rec, 0, sizeof(rec));
	rec.ticks = arch_time_uptime_ticks();
	rec.level = (uint8_t)level;
	k_strncpy(rec.tag, tag ? tag : "KLOG", KLOG_TAG_CAP - 1u);
	rec.tag[KLOG_TAG_CAP - 1u] = '\0';
	k_strncpy(rec.msg, msg ? msg : "(null)", KLOG_MSG_CAP - 1u);
	rec.msg[KLOG_MSG_CAP - 1u] = '\0';

	klog_store_record(&rec);
	klog_emit_record_silent(&rec);
}

void klog_silent(const char *tag, const char *msg)
{
	klog_log_silent(KLOG_LEVEL_INFO, tag, msg);
}

void klog_silent_uint(const char *tag, const char *msg, uint32_t val)
{
	char line[KLOG_MSG_CAP];

	k_snprintf(line, sizeof(line), "%s: %u", msg ? msg : "(null)", val);
	klog_log_silent(KLOG_LEVEL_INFO, tag, line);
}

void klog_silent_hex(const char *tag, const char *msg, uint32_t val)
{
	char line[KLOG_MSG_CAP];

	k_snprintf(line, sizeof(line), "%s: 0x%08X", msg ? msg : "(null)", val);
	klog_log_silent(KLOG_LEVEL_INFO, tag, line);
}

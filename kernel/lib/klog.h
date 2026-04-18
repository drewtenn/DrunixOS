/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

typedef enum {
    KLOG_LEVEL_DEBUG = 0,
    KLOG_LEVEL_INFO  = 1,
    KLOG_LEVEL_WARN  = 2,
    KLOG_LEVEL_ERROR = 3,
} klog_level_t;

/*
 * klog — structured kernel log with a retained in-memory history.
 *
 * The legacy helpers below remain the common entry points and default to
 * INFO-level records. Each record is emitted to the VGA/debugcon consoles
 * and also retained in a fixed-size in-kernel ring buffer for procfs/dmesg.
 *
 * Rendered log lines include:
 *   - a monotonic timestamp since boot
 *   - a severity level
 *   - a subsystem tag
 *   - the message text
 *
 * These calls are permanent infrastructure, not temporary debug prints.
 * Add them wherever a subsystem initialises or changes important state.
 */

void klog_log(klog_level_t level, const char *tag, const char *msg);
void klog(const char *tag, const char *msg);
void klog_uint(const char *tag, const char *msg, uint32_t val);
void klog_hex(const char *tag, const char *msg, uint32_t val);

/*
 * Silent variants — store to the ring and emit to the QEMU debugcon port,
 * but never write to the on-screen console. Used by the in-kernel unit
 * test suite so that a KTEST build's boot screen stays visually identical
 * to a non-KTEST build.
 */
void klog_log_silent(klog_level_t level, const char *tag, const char *msg);
void klog_silent(const char *tag, const char *msg);
void klog_silent_uint(const char *tag, const char *msg, uint32_t val);
void klog_silent_hex(const char *tag, const char *msg, uint32_t val);

/*
 * Snapshot the retained ring buffer into `buf`.
 *
 * Returns the number of rendered bytes via `size_out` even when `buf` is
 * NULL. Output is always textual and newline-delimited, suitable for
 * procfs exposure.
 */
int klog_snapshot(char *buf, uint32_t cap, uint32_t *size_out);

#endif

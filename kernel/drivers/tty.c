/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * tty.c — terminal line discipline, canonical input, and job-control signaling.
 */

#include "tty.h"
#include "desktop.h"
#include "../proc/sched.h"
#include "kstring.h"
#include <stdint.h>

extern void print_string(char *string);
extern void print_char(char c);

void sched_send_signal_to_pgid(uint32_t pgid, int signum);

static tty_t tty_table[MAX_TTYS];

static void tty_feedback(const char *buf, uint32_t len)
{
    desktop_state_t *desktop = desktop_is_active() ? desktop_global() : 0;

    if (desktop && desktop_write_console_output(desktop, buf, len) == (int)len)
        return;

    print_string((char *)buf);
}

/* ── public API ─────────────────────────────────────────────────────────── */

void tty_init(void)
{
    int i;
    for (i = 0; i < MAX_TTYS; i++) {
        tty_t *t = &tty_table[i];
        t->raw_head = t->raw_tail = 0;
        t->canon_len = 0;
        t->canon_ready = 0;
        t->termios.c_lflag = 0; /* raw, no echo — preserves shell readline */
        t->ctrl_sid = 0;
        t->fg_pgid = 0;
        t->read_waiters.head = 0;
        t->read_waiters.tail = 0;
        t->in_use  = 1;
    }
}

tty_t *tty_get(int tty_idx)
{
    if (tty_idx < 0 || tty_idx >= MAX_TTYS) return 0;
    if (!tty_table[tty_idx].in_use)         return 0;
    return &tty_table[tty_idx];
}

void tty_wake_readers(int tty_idx)
{
    tty_t *tty = tty_get(tty_idx);
    if (!tty) return;
    sched_wake_all(&tty->read_waiters);
}

/*
 * tty_input_char — called from the keyboard IRQ handler (interrupt context).
 *
 * Routes the character through the line discipline:
 *   - Ctrl+C (0x03) with ISIG set → tty_ctrl_c()
 *   - ICANON set  → append to canonical buffer; echo if ECHO; on \n set
 *                   canon_ready and wake blocked readers
 *   - raw mode    → push to raw ring buffer and wake readers
 */
void tty_input_char(int tty_idx, char c)
{
    tty_t *tty = tty_get(tty_idx);
    if (!tty) return;

    /* Ctrl+C */
    if (c == 0x03 && (tty->termios.c_lflag & ISIG)) {
        tty_ctrl_c(tty_idx);
        return;
    }

    /* Ctrl+Z (SUB, 0x1A) */
    if (c == 0x1A && (tty->termios.c_lflag & ISIG)) {
        tty_ctrl_z(tty_idx);
        return;
    }

    if (tty->termios.c_lflag & ICANON) {
        /* Canonical mode */
        if (c == '\b' || c == 0x7f) {
            /* Backspace / DEL: erase last character */
            if (tty->canon_len > 0) {
                tty->canon_len--;
                if (tty->termios.c_lflag & ECHOE) {
                    print_string("\b \b");
                }
            }
        } else {
            if (tty->canon_len < TTY_CANON_BUF_SIZE - 1) {
                tty->canon_buf[tty->canon_len++] = c;
                if (tty->termios.c_lflag & ECHO) {
                    /* Echo the character */
                    char s[2] = {c, '\0'};
                    print_string(s);
                }
            }
            if (c == '\n') {
                tty->canon_ready = 1;
                tty_wake_readers(tty_idx);
            }
        }
    } else {
        /* Raw mode: push to ring buffer */
        uint32_t next = (tty->raw_head + 1) % TTY_RAW_BUF_SIZE;
        if (next != tty->raw_tail) { /* drop if full */
            tty->raw_buf[tty->raw_head] = c;
            tty->raw_head = next;
        }
        tty_wake_readers(tty_idx);
    }
}

void tty_ctrl_z(int tty_idx)
{
    tty_t *tty = tty_get(tty_idx);
    if (!tty) return;

    if (tty->fg_pgid != 0) {
        tty_feedback("^Z\n", 3);
        sched_send_signal_to_pgid(tty->fg_pgid, SIGTSTP);
    }
    /* If no fg_pgid, discard — there is no foreground process to stop. */
}

void tty_ctrl_c(int tty_idx)
{
    tty_t *tty = tty_get(tty_idx);
    if (!tty) return;

    if (tty->fg_pgid == 0) return;

    tty_feedback("^C\n", 3);
    sched_send_signal_to_pgid(tty->fg_pgid, SIGINT);
}

/*
 * tty_read — block the current process until data is available, then copy
 * up to `count` bytes into `buf`.
 *
 * Returns bytes copied (>= 1), or -1 if a signal is pending.
 */
int tty_read(int tty_idx, char *buf, uint32_t count)
{
    tty_t *tty = tty_get(tty_idx);
    if (!tty || count == 0) return -1;

    for (;;) {
        process_t *cur = sched_current();
        if (!cur) return -1;

        /* Signal interrupt */
        if (cur->sig_pending)
            return -1;

        if (tty->termios.c_lflag & ICANON) {
            if (tty->canon_ready) {
                /* Copy up to count bytes from the line buffer */
                uint32_t n = tty->canon_len < count ? tty->canon_len : count;
                k_memcpy(buf, tty->canon_buf, n);

                /* Shift remaining bytes left */
                uint32_t remaining = tty->canon_len - n;
                k_memmove(tty->canon_buf, tty->canon_buf + n, remaining);
                tty->canon_len = remaining;
                if (remaining == 0)
                    tty->canon_ready = 0;

                return (int)n;
            }
        } else {
            /* Raw mode: drain available bytes */
            if (tty->raw_head != tty->raw_tail) {
                uint32_t n = 0;
                while (n < count && tty->raw_head != tty->raw_tail) {
                    buf[n++] = tty->raw_buf[tty->raw_tail];
                    tty->raw_tail = (tty->raw_tail + 1) % TTY_RAW_BUF_SIZE;
                }
                return (int)n;
            }
        }

        /* No data available — block until the keyboard IRQ wakes us */
        sched_block(&tty->read_waiters);
        /* After waking, loop again to re-check */
    }
}

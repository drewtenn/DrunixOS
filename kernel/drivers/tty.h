/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TTY_H
#define TTY_H

#include "../proc/wait.h"
#include <stdint.h>

/* ── termios input-flag bits (c_iflag) ─────────────────────────────────── */
#define ICRNL (1u << 0) /* map CR to NL on input                    */

/* ── termios output-flag bits (c_oflag) ────────────────────────────────── */
#define OPOST (1u << 0) /* post-process output                      */
#define ONLCR (1u << 1) /* map NL to CR-NL on output                */

/* ── termios control-flag bits (c_cflag) ───────────────────────────────── */
#define CREAD (1u << 0) /* enable receiver                          */
#define CS8 (1u << 1)   /* 8-bit characters                         */

/* ── termios local-flag bits (c_lflag) ─────────────────────────────────── */
#define ICANON (1u << 0) /* canonical (line-buffered) mode            */
#define ECHO (1u << 1)   /* echo each input character back            */
#define ECHOE (1u << 2)  /* echo ERASE as BS SP BS                    */
#define ISIG (1u << 3)   /* generate SIGINT on Ctrl+C                 */

/* ── tcsetattr `action` values ─────────────────────────────────────────── */
#define TCSANOW 0   /* apply change immediately                        */
#define TCSAFLUSH 2 /* apply change after flushing unread input        */

#define NCCS 19
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10

/* Minimal termios struct with Linux-shaped fields used by real TTY apps. */
typedef struct {
	uint32_t c_iflag;
	uint32_t c_oflag;
	uint32_t c_cflag;
	uint32_t c_lflag;
	uint8_t c_cc[NCCS];
} termios_t;

/* ── TTY line discipline ────────────────────────────────────────────────── */
#define TTY_RAW_BUF_SIZE 256
#define TTY_CANON_BUF_SIZE 256
#define MAX_TTYS 4

typedef struct {
	/* Raw ring buffer: used in raw mode (ICANON clear).
     * Filled by the keyboard IRQ; drained by tty_read(). */
	char raw_buf[TTY_RAW_BUF_SIZE];
	uint32_t raw_head; /* write index (IRQ side)  */
	uint32_t raw_tail; /* read  index (read side) */

	/* Canonical line buffer: used in canonical mode (ICANON set).
     * Characters accumulate here until a newline arrives; at that point
     * canon_ready is set to 1 and waiting readers are woken. */
	char canon_buf[TTY_CANON_BUF_SIZE];
	uint32_t canon_len;   /* bytes assembled so far (including \n)      */
	uint32_t canon_ready; /* 1 = a complete line is waiting to be read  */

	termios_t termios; /* current terminal settings                    */
	uint32_t ctrl_sid; /* controlling session ID (0 = unclaimed)       */
	uint32_t fg_pgid;  /* foreground process group ID (0 = none set)   */
	uint32_t interrupted; /* VINTR/VSUSP interrupted a blocked read      */
	wait_queue_t read_waiters; /* readers blocked for line/ring-buffer input */
	uint32_t in_use; /* 1 if this slot is active                      */
} tty_t;

/*
 * tty_init: zero-initialise the TTY table and mark the virtual TTY slots active.
 * Default mode follows a normal Linux terminal: canonical input, echo, signal
 * keys, ICRNL, and standard control characters.  Must be called before
 * keyboard_init().
 */
void tty_init(void);

/*
 * tty_input_char: process one decoded character from the keyboard IRQ.
 * Safe to call from interrupt context.  Handles special characters
 * (Ctrl+C, backspace) and routes to the raw ring or canonical buffer
 * according to termios.c_lflag.
 */
void tty_input_char(int tty_idx, char c);

/*
 * tty_read_available: return the number of bytes currently readable without
 * blocking from the selected TTY, honoring canonical vs raw mode.
 */
uint32_t tty_read_available(int tty_idx);

/*
 * tty_ctrl_c: called when Ctrl+C is pressed.
 * If a foreground process group is installed, sends SIGINT to it.
 */
void tty_ctrl_c(int tty_idx);

/*
 * tty_ctrl_z: called when Ctrl+Z is pressed.
 * If ISIG is set and fg_pgid != 0, sends SIGTSTP to the foreground process
 * group.  Otherwise the keystroke is discarded.
 */
void tty_ctrl_z(int tty_idx);

/*
 * tty_read: copy up to `count` bytes from the TTY into `buf`.
 * Blocks the current process on the TTY read wait queue until data arrives.
 * Returns the number of bytes copied (>= 1), or -1 if interrupted by a
 * pending signal.
 */
int tty_read(int tty_idx, char *buf, uint32_t count);

/*
 * tty_get: return a pointer to the tty_t at the given index, or NULL if
 * the index is out of range or the slot is not in_use.
 */
tty_t *tty_get(int tty_idx);

/*
 * tty_wake_readers: wake readers blocked on `tty_idx`.
 */
void tty_wake_readers(int tty_idx);

#endif /* TTY_H */

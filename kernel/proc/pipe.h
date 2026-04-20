/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PIPE_H
#define PIPE_H

#include "wait.h"
#include <stdint.h>

#define PIPE_BUF_SIZE 4096u /* bytes per pipe ring buffer           */
#define MAX_PIPES 8u        /* maximum simultaneous kernel pipes    */

/*
 * pipe_buf_t — one kernel pipe.
 *
 * The ring buffer is managed with read_idx, write_idx, and count.
 * Using an explicit count avoids the full/empty ambiguity that arises
 * when read_idx == write_idx is used to mean both states.
 *
 * read_open / write_open track how many file-descriptor ends are still
 * open across all processes.  When write_open reaches 0, a reader sees
 * EOF.  When read_open reaches 0, a writer gets a broken-pipe error.
 */
typedef struct {
	uint8_t buf[PIPE_BUF_SIZE];
	uint32_t read_idx;    /* index of the next byte to read    */
	uint32_t write_idx;   /* index of the next byte to write   */
	uint32_t count;       /* bytes currently in the buffer     */
	uint32_t read_open;   /* open read-end fds (all processes) */
	uint32_t write_open;  /* open write-end fds (all processes)*/
	wait_queue_t waiters; /* readers and writers blocked on this pipe */
	uint32_t in_use;      /* 1 if this slot is allocated       */
} pipe_buf_t;

/*
 * pipe_alloc: claim a free slot, initialise it, and return its index
 * (0 – MAX_PIPES-1).  Returns -1 if the table is full.
 */
int pipe_alloc(void);

/*
 * pipe_free: release the slot at idx.  Callers must ensure both
 * read_open and write_open have reached 0 before calling this.
 */
void pipe_free(int idx);

/*
 * pipe_get: return a pointer to the pipe_buf_t at idx, or NULL if idx
 * is out of range or the slot is not in use.
 */
pipe_buf_t *pipe_get(int idx);

#endif

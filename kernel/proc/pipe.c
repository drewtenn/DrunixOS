/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pipe.c — kernel pipe buffer allocation and blocking pipe I/O primitives.
 */

#include "pipe.h"

static pipe_buf_t pipe_table[MAX_PIPES];

int pipe_alloc(void)
{
    for (unsigned i = 0; i < MAX_PIPES; i++) {
        if (!pipe_table[i].in_use) {
            pipe_buf_t *p = &pipe_table[i];
            p->read_idx   = 0;
            p->write_idx  = 0;
            p->count      = 0;
            p->read_open  = 1;
            p->write_open = 1;
            p->waiters.head = 0;
            p->waiters.tail = 0;
            p->in_use     = 1;
            return (int)i;
        }
    }
    return -1;
}

void pipe_free(int idx)
{
    if (idx >= 0 && (unsigned)idx < MAX_PIPES)
        pipe_table[idx].in_use = 0;
}

pipe_buf_t *pipe_get(int idx)
{
    if (idx < 0 || (unsigned)idx >= MAX_PIPES)
        return 0;
    if (!pipe_table[idx].in_use)
        return 0;
    return &pipe_table[idx];
}

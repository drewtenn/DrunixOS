/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef WAIT_H
#define WAIT_H

typedef struct process process_t;

/*
 * wait_queue_t — intrusive queue head for processes blocked on a shared event.
 *
 * The queue object itself is the wait channel identity: pipes, TTYs, and
 * per-process child-state notifications each embed one of these.
 */
typedef struct {
    process_t *head;
    process_t *tail;
} wait_queue_t;

#endif

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pty.c — pseudo-terminal pair pool.
 */

#include "pty.h"
#include "sched.h"
#include <stdint.h>

#define PTY_RING_BYTES 1024u

typedef struct {
	uint8_t buf[PTY_RING_BYTES];
	uint32_t head;
	uint32_t tail;
	uint32_t count;
	uint32_t closed;
	wait_queue_t waiters;
} pty_ring_t;

typedef struct {
	uint32_t in_use;
	uint32_t master_open;
	uint32_t slave_open;
	pty_ring_t to_slave; /* master writes, slave reads */
	pty_ring_t to_master; /* slave writes, master reads */
} pty_t;

static pty_t pty_pool[PTY_MAX];

static void pty_ring_reset(pty_ring_t *r)
{
	r->head = 0;
	r->tail = 0;
	r->count = 0;
	r->closed = 0;
	sched_wait_queue_init(&r->waiters);
}

static int pty_ring_write(pty_ring_t *r, const uint8_t *buf, uint32_t count)
{
	uint32_t copied = 0;

	if (!r || !buf || count == 0)
		return 0;

	while (copied < count) {
		while (r->count == PTY_RING_BYTES) {
			if (r->closed)
				return (int)copied;
			sched_block(&r->waiters);
		}
		while (copied < count && r->count < PTY_RING_BYTES) {
			r->buf[r->head] = buf[copied++];
			r->head = (r->head + 1u) % PTY_RING_BYTES;
			r->count++;
		}
		sched_wake_all(&r->waiters);
	}
	return (int)copied;
}

static int pty_ring_read(pty_ring_t *r, uint8_t *buf, uint32_t count)
{
	uint32_t copied = 0;

	if (!r || !buf || count == 0)
		return 0;

	while (r->count == 0) {
		if (r->closed)
			return 0;
		sched_block(&r->waiters);
	}

	while (copied < count && r->count > 0) {
		buf[copied++] = r->buf[r->tail];
		r->tail = (r->tail + 1u) % PTY_RING_BYTES;
		r->count--;
	}
	sched_wake_all(&r->waiters);
	return (int)copied;
}

int pty_alloc_master(void)
{
	for (uint32_t i = 0; i < PTY_MAX; i++) {
		pty_t *p = &pty_pool[i];
		if (p->in_use)
			continue;

		p->in_use = 1;
		p->master_open = 1;
		p->slave_open = 0;
		pty_ring_reset(&p->to_slave);
		pty_ring_reset(&p->to_master);
		return (int)i;
	}
	return -1;
}

int pty_open_slave(uint32_t pty_idx)
{
	pty_t *p;

	if (pty_idx >= PTY_MAX)
		return -1;
	p = &pty_pool[pty_idx];
	if (!p->in_use || !p->master_open || p->slave_open)
		return -1;

	p->slave_open = 1;
	return 0;
}

void pty_release_master(uint32_t pty_idx)
{
	pty_t *p;

	if (pty_idx >= PTY_MAX)
		return;
	p = &pty_pool[pty_idx];
	if (!p->in_use)
		return;

	p->master_open = 0;
	p->to_slave.closed = 1;
	p->to_master.closed = 1;
	sched_wake_all(&p->to_slave.waiters);
	sched_wake_all(&p->to_master.waiters);

	if (!p->slave_open)
		p->in_use = 0;
}

void pty_release_slave(uint32_t pty_idx)
{
	pty_t *p;

	if (pty_idx >= PTY_MAX)
		return;
	p = &pty_pool[pty_idx];
	if (!p->in_use)
		return;

	p->slave_open = 0;
	p->to_slave.closed = 1;
	p->to_master.closed = 1;
	sched_wake_all(&p->to_slave.waiters);
	sched_wake_all(&p->to_master.waiters);

	if (!p->master_open)
		p->in_use = 0;
}

int pty_master_read(uint32_t pty_idx, uint8_t *buf, uint32_t count)
{
	if (pty_idx >= PTY_MAX || !pty_pool[pty_idx].in_use)
		return -1;
	return pty_ring_read(&pty_pool[pty_idx].to_master, buf, count);
}

int pty_master_write(uint32_t pty_idx, const uint8_t *buf, uint32_t count)
{
	if (pty_idx >= PTY_MAX || !pty_pool[pty_idx].in_use)
		return -1;
	return pty_ring_write(&pty_pool[pty_idx].to_slave, buf, count);
}

int pty_slave_read(uint32_t pty_idx, uint8_t *buf, uint32_t count)
{
	if (pty_idx >= PTY_MAX || !pty_pool[pty_idx].in_use)
		return -1;
	return pty_ring_read(&pty_pool[pty_idx].to_slave, buf, count);
}

int pty_slave_write(uint32_t pty_idx, const uint8_t *buf, uint32_t count)
{
	if (pty_idx >= PTY_MAX || !pty_pool[pty_idx].in_use)
		return -1;
	return pty_ring_write(&pty_pool[pty_idx].to_master, buf, count);
}

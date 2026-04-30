/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * netdev.c — /dev/net0 raw Ethernet frame chardev.
 *
 * M4 commit 6. Thin shim over the chardev framework. A transport
 * driver (arm64 virtio-net is the only consumer in v1) registers an
 * ops table; the chardev's read/write dispatch through it. Read
 * blocks on a wait queue until the transport calls netdev_signal_rx
 * after pushing a frame into its RX ring.
 *
 * Frame protocol: each read returns exactly one Ethernet frame
 * (transport has stripped any link-level header like virtio_net_hdr).
 * Each write submits exactly one frame. Lengths are 14..1514.
 */

#include "netdev.h"
#include "chardev.h"
#include "sched.h"
#include <stdint.h>

static const netdev_ops_t *g_ops;
static wait_queue_t g_rx_waiters;

void netdev_signal_rx(void)
{
	sched_wake_all(&g_rx_waiters);
}

int netdev_register(const netdev_ops_t *ops)
{
	if (!ops || !ops->recv_frame || !ops->send_frame)
		return -1;
	if (g_ops)
		return -1;
	g_ops = ops;
	return 0;
}

/*
 * Blocking read: pull one Ethernet frame from the transport into
 * `buf`. Sleeps on the RX wait queue until a frame is available.
 * Returns -1 if the caller's buffer is too small for the head-of-
 * queue frame (slot stays head-of-queue; userspace can retry).
 */
static int net0_read(uint32_t offset, uint8_t *buf, uint32_t count)
{
	int32_t n;

	(void)offset;

	if (!g_ops)
		return -1;

	for (;;) {
		n = g_ops->recv_frame(buf, count);
		if (n != 0)
			return (int)n;
		sched_block(&g_rx_waiters);
	}
}

/*
 * Non-blocking write: submit one raw Ethernet frame. Returns the
 * byte count submitted on success, -1 on length out of range or
 * transport-busy. Userspace can retry busy returns.
 */
static int net0_write(uint32_t offset, const uint8_t *buf, uint32_t count)
{
	int32_t n;

	(void)offset;

	if (!g_ops)
		return -1;

	n = g_ops->send_frame(buf, count);
	return (int)n;
}

static const chardev_ops_t net0_chardev_ops = {
	.read_char = 0,
	.write_char = 0,
	.write = net0_write,
	.read = net0_read,
	.mmap_phys = 0,
	.mmap_cache_policy = 0,
	.ioctl = 0,
};

int netdev_init(void)
{
	sched_wait_queue_init(&g_rx_waiters);
	return chardev_register("net0", &net0_chardev_ops);
}

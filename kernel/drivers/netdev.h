/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRIVERS_NETDEV_H
#define DRIVERS_NETDEV_H

#include <stdint.h>

/*
 * /dev/net0: raw Ethernet frame chardev.
 *
 * M4 commit 6. The shared netdev layer is a thin shim over the
 * chardev framework, analogous to inputdev for /dev/kbd and
 * /dev/mouse. It owns:
 *
 *   - the /dev/net0 chardev registration
 *   - a wait queue that read(2) blocks on when no frame is ready
 *   - an ops-table pointer that a transport driver (currently
 *     arm64 virtio-net) registers itself through
 *
 * The transport driver provides recv_frame and send_frame functions
 * via netdev_register(); netdev's chardev read/write dispatch them.
 *
 * Frame protocol on /dev/net0: each read returns exactly one
 * Ethernet frame (no virtio_net_hdr — the transport strips it).
 * Each write submits exactly one frame. Frames are 14..1514 bytes
 * (Ethernet II minimum 14 byte header to MTU 1514).
 *
 * Why one device today: M4 only supports a single virtio-net
 * device. A future revision can expand the registry to /dev/netN
 * by parameterising the name.
 */

#define NETDEV_MAX_FRAME 1514u

typedef struct {
	/*
	 * Pull one Ethernet frame from the transport's RX queue into
	 * `out`. Returns the byte count (1..NETDEV_MAX_FRAME) on success,
	 * 0 if no frame is available (caller should sleep), or -1 if
	 * `max_len` is too small to hold the head-of-queue frame (slot
	 * is NOT consumed; caller should retry with a larger buffer).
	 */
	int32_t (*recv_frame)(uint8_t *out, uint32_t max_len);

	/*
	 * Submit one raw Ethernet frame for transmission. `frame` points
	 * to `len` bytes of Ethernet (no virtio_net_hdr — the transport
	 * prepends one if its protocol requires it). Returns the byte
	 * count submitted on success, -1 on length-out-of-range or
	 * resource exhaustion. Single-producer; caller serializes.
	 */
	int32_t (*send_frame)(const uint8_t *frame, uint32_t len);
} netdev_ops_t;

/*
 * Initialize netdev: set up the wait queue and register the /dev/net0
 * chardev. Safe to call when no transport has registered yet — read
 * will block until a transport registers, but unblocks on signal_rx.
 * Returns 0 on success, -1 if chardev registration fails.
 */
int netdev_init(void);

/*
 * A transport driver registers its ops table here, exactly once. The
 * arm64 virtio-net driver calls this from its init path after
 * DRIVER_OK. Returns 0 on success, -1 if a transport was already
 * registered.
 */
int netdev_register(const netdev_ops_t *ops);

/*
 * Called from the transport's IRQ path after a frame has been
 * pushed into the RX ring, to wake any reader blocked in read().
 * Safe in IRQ context.
 */
void netdev_signal_rx(void);

#endif /* DRIVERS_NETDEV_H */

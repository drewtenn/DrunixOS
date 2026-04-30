/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * net0send - M4 commit 7 smoke tool: write one raw Ethernet frame.
 *
 * Builds a deterministic 14-byte Ethernet header + 14-byte payload
 * frame and writes it once to /dev/net0. Used by the integration
 * KTEST harness (commit 8) to verify host-side observation of guest
 * TX. The destination MAC is the harness peer's address; the source
 * MAC matches the guest's configured virtio-net MAC.
 *
 * EtherType 0x88b5 is reserved for local experimental use under the
 * IEEE 802 framework, so the frame cannot collide with any real
 * higher-layer protocol.
 */

#include "stdio.h"
#include "string.h"
#include "syscall.h"
#include <stdint.h>

#define NET0_PATH        "/dev/net0"
#define NET0_PAYLOAD     "drunix-m4-ping"
#define NET0_PAYLOAD_LEN 14

int main(void)
{
	uint8_t frame[14 + NET0_PAYLOAD_LEN];
	int fd;
	int n;

	/* Destination MAC: 52:54:00:0d:00:02 (harness peer). */
	frame[0] = 0x52; frame[1] = 0x54; frame[2] = 0x00;
	frame[3] = 0x0d; frame[4] = 0x00; frame[5] = 0x02;

	/* Source MAC: 52:54:00:0d:00:01 (configured guest MAC). */
	frame[6] = 0x52; frame[7] = 0x54; frame[8] = 0x00;
	frame[9] = 0x0d; frame[10] = 0x00; frame[11] = 0x01;

	/* EtherType: 0x88b5 (local experimental, big-endian on the wire). */
	frame[12] = 0x88;
	frame[13] = 0xb5;

	memcpy(frame + 14, NET0_PAYLOAD, NET0_PAYLOAD_LEN);

	fd = sys_open_flags(NET0_PATH, 1 /* O_WRONLY */, 0);
	if (fd < 0) {
		printf("net0send: cannot open %s\n", NET0_PATH);
		return 1;
	}

	n = sys_fwrite(fd, (const char *)frame, (int)sizeof(frame));
	if (n != (int)sizeof(frame)) {
		printf("net0send: write returned %d (expected %d)\n",
		       n, (int)sizeof(frame));
		return 2;
	}

	printf("net0send: wrote %d bytes to %s\n", n, NET0_PATH);
	return 0;
}

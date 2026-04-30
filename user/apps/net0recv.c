/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * net0recv - M4 commit 7 smoke tool: read one raw Ethernet frame.
 *
 * Opens /dev/net0, blocks on read until one frame arrives, validates
 * the minimum Ethernet header length (14 bytes), and prints the
 * destination MAC, source MAC, EtherType, and payload as hex. Exits
 * with status 0 on a successful read, non-zero on error.
 *
 * Pairs with the M4 commit 8 integration KTEST harness, which
 * injects a frame from the host side and verifies net0recv's
 * observation matches byte-for-byte.
 */

#include "stdio.h"
#include "syscall.h"
#include <stdint.h>

#define NET0_PATH      "/dev/net0"
#define MAX_FRAME_LEN  1514

int main(void)
{
	uint8_t frame[MAX_FRAME_LEN];
	int fd;
	int n;

	fd = sys_open_flags(NET0_PATH, 0 /* O_RDONLY */, 0);
	if (fd < 0) {
		printf("net0recv: cannot open %s\n", NET0_PATH);
		return 1;
	}

	n = sys_read(fd, (char *)frame, (int)sizeof(frame));
	if (n < 14) {
		printf("net0recv: read returned %d (expected >=14)\n", n);
		return 2;
	}

	printf("net0recv: %d bytes\n", n);
	printf("  dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
	       frame[0], frame[1], frame[2],
	       frame[3], frame[4], frame[5]);
	printf("  src=%02x:%02x:%02x:%02x:%02x:%02x\n",
	       frame[6], frame[7], frame[8],
	       frame[9], frame[10], frame[11]);
	printf("  type=0x%02x%02x\n", frame[12], frame[13]);

	printf("  payload=");
	for (int i = 14; i < n; i++)
		printf("%02x", frame[i]);
	printf("\n");

	return 0;
}

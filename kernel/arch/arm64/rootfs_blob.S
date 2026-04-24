/* SPDX-License-Identifier: GPL-3.0-or-later */

.section .rodata.arm64_rootfs, "a", %progbits
.balign 512
.global arm64_rootfs_start
.global arm64_rootfs_end
arm64_rootfs_start:
	.incbin "build/arm64-root.fs"
arm64_rootfs_end:
.balign 512

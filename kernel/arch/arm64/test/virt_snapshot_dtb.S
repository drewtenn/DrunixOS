/* SPDX-License-Identifier: GPL-3.0-or-later */

.section .rodata, "a", @progbits
.globl virt_snapshot_dtb
.type virt_snapshot_dtb, @object
.balign 8
virt_snapshot_dtb:
    .incbin "tools/virt-snapshot.dtb"
1:
.size virt_snapshot_dtb, 1b - virt_snapshot_dtb

.globl virt_snapshot_dtb_size
.type virt_snapshot_dtb_size, @object
.balign 4
virt_snapshot_dtb_size:
    .word 1b - virt_snapshot_dtb
.size virt_snapshot_dtb_size, 4

/* SPDX-License-Identifier: GPL-3.0-or-later */

.section .text
.global _start
_start:
	bl main
	mov x8, #93
	mov x0, x0
	svc #0
1:
	b 1b

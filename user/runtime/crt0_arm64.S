/* SPDX-License-Identifier: GPL-3.0-or-later */

.section .text
	.global _start
	.extern main
	.extern sys_exit
	.extern environ
	.extern __drunix_run_constructors
	.extern __drunix_run_destructors

_start:
	mov x19, sp
	ldr x20, [x19]          // argc
	add x21, x19, #8        // argv
	add x22, x21, x20, lsl #3
	add x22, x22, #8        // envp

	adrp x0, environ
	add x0, x0, :lo12:environ
	str x22, [x0]

	bl __drunix_run_constructors

	mov x0, x20
	mov x1, x21
	mov x2, x22
	bl main
	mov x19, x0

	bl __drunix_run_destructors

	mov x0, x19
	bl sys_exit
1:
	wfe
	b 1b

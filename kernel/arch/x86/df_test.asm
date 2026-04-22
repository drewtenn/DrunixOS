; SPDX-License-Identifier: GPL-3.0-or-later
; df_test.asm — intentional crash harness for verifying the dedicated double-fault path.

[bits 32]

; trigger_double_fault: intentionally destroy the current ring-0 stack and
; execute an invalid instruction. Delivering the #UD frame on an unusable
; kernel stack forces the CPU down the double-fault task-gate path.

global trigger_double_fault
trigger_double_fault:
    cli
    xor eax, eax
    mov esp, eax
    ud2

.halt:
    cli
    hlt
    jmp .halt

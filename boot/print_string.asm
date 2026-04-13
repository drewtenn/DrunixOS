; SPDX-License-Identifier: GPL-3.0-or-later
; print_string.asm — BIOS-based real-mode string output helper for early boot diagnostics.

print_string:
    pusha
print_string_loop:
    mov ah, 0x0e
    mov al, [bx]
    cmp al, 0
    je print_string_done
    add bx, 1
    int 0x10
    jmp print_string_loop
print_string_done:
    popa
    ret

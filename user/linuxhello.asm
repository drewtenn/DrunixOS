; SPDX-License-Identifier: GPL-3.0-or-later
; linuxhello.asm — static i386 Linux ABI smoke binary.

BITS 32

global _start

section .text
_start:
    mov eax, 4          ; Linux i386 SYS_write
    mov ebx, 1          ; stdout
    mov ecx, message
    mov edx, message_len
    int 0x80

    mov eax, 1          ; Linux i386 SYS_exit
    xor ebx, ebx
    int 0x80

section .rodata
message:
    db "hello from linux", 10
message_len equ $ - message

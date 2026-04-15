; SPDX-License-Identifier: GPL-3.0-or-later
; Multiboot1 header — must be in the first 8 KB of the kernel image
section .multiboot
align 4
    MULTIBOOT_MAGIC    equ 0x1BADB002
    MULTIBOOT_FLAGS    equ 0x04
    MULTIBOOT_CHECKSUM equ -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

    dd MULTIBOOT_MAGIC
    dd MULTIBOOT_FLAGS
    dd MULTIBOOT_CHECKSUM
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd 1024
    dd 768
    dd 32

; 16 KB kernel stack in BSS
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
[bits 32]
global _start

_start:
    ; GRUB enters here with:
    ;   EAX = 0x2BADB002  (multiboot magic)
    ;   EBX = physical address of multiboot_info_t
    ; Set up our own stack immediately so we don't rely on the bootloader's.
    mov esp, stack_top

    ; Push arguments for start_kernel(uint32_t magic, multiboot_info_t *mbi)
    push ebx    ; arg2: multiboot info pointer
    push eax    ; arg1: magic number

    extern start_kernel
    call start_kernel

    ; Should never return — halt if it does
    jmp $

; SPDX-License-Identifier: GPL-3.0-or-later
; paging.asm
; CR3 and CR0 require register operands — inline asm in C is too awkward.

[bits 32]

global paging_load_cr3
paging_load_cr3:
    mov eax, [esp + 4]   ; first argument: physical address of page directory
    mov cr3, eax
    ret

global paging_enable
paging_enable:
    mov eax, cr0
    or  eax, 0x80000000  ; set bit 31 (PG flag)
    mov cr0, eax
    ret

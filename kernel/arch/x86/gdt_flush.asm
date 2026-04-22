; SPDX-License-Identifier: GPL-3.0-or-later
; gdt_flush.asm — assembly helper that loads the installed GDT and task register.

[bits 32]

; void gdt_flush(uint32_t gdtr_ptr, uint32_t tss_selector);
;
; Loads a new GDTR, reloads all segment registers from the new GDT, performs
; a far jump to flush the instruction pipeline (required to reload CS), and
; installs the TSS via LTR.
;
; The far jump uses an absolute address with the kernel code selector 0x08.
; After the jump the CPU fetches from the new GDT's code descriptor.
;
; All parameters are read into registers before any stack manipulation to
; avoid offset miscalculation during the reload sequence.
global gdt_flush
gdt_flush:
    mov eax, [esp+4]        ; gdtr_ptr
    mov ecx, [esp+8]        ; tss_selector

    lgdt [eax]              ; install new GDT

    ; Reload all data-class segment registers with kernel data selector 0x10
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to flush the instruction pipeline and reload CS.
    ; The CPU will re-read the CS descriptor from the new GDT after this jump.
    jmp 0x08:.flush_cs
.flush_cs:

    ; Install TSS — informs the CPU which GDT entry describes our TSS
    mov ax, cx              ; tss_selector = 0x28
    ltr ax

    ret

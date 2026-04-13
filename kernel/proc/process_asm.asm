; SPDX-License-Identifier: GPL-3.0-or-later
; process_asm.asm — assembly trampoline for entering a new user-mode process with iret.

[bits 32]

; void process_enter_usermode(uint32_t entry,    [esp+4]
;                              uint32_t user_esp, [esp+8]
;                              uint32_t user_cs,  [esp+12]
;                              uint32_t user_ds); [esp+16]
;
; Transfers execution to ring-3 code by constructing an iret frame on the
; kernel stack and executing iret.
;
; The CPU's iret for a CPL change pops (low address first):
;   EIP, CS, EFLAGS, ESP, SS
;
; We push them in reverse order (SS first) so iret finds them in the right
; order.  All four arguments are loaded into registers BEFORE any stack
; manipulation to avoid offset miscalculation.
global process_enter_usermode
process_enter_usermode:
    ; Save all arguments to registers first
    mov ecx, [esp+4]    ; entry   (EIP for ring 3)
    mov edx, [esp+8]    ; user_esp
    mov ebx, [esp+12]   ; user_cs = GDT_USER_CS = 0x1B
    mov eax, [esp+16]   ; user_ds = GDT_USER_DS = 0x23

    ; Load user data segment into all data-class registers now, while still
    ; in ring 0.  The CPU will re-validate them on the iret transition.
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build the iret frame on the current (kernel) stack:
    ;   push SS    — ring-3 stack segment
    ;   push ESP   — ring-3 initial stack pointer
    ;   push EFLAGS — copied from current flags with IF set
    ;   push CS    — ring-3 code segment
    ;   push EIP   — entry point
    push eax                    ; SS  = user_ds (0x23)
    push edx                    ; ESP = user_esp
    pushfd
    or dword [esp], 0x200       ; IF=1: enable hardware interrupts in ring 3
    push ebx                    ; CS  = user_cs (0x1B)
    push ecx                    ; EIP = entry
    iret                        ; atomically: load EIP/CS, switch CPL, load ESP/SS

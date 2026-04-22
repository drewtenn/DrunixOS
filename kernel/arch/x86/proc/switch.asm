; SPDX-License-Identifier: GPL-3.0-or-later
;
; switch_context / process_initial_launch
;
; switch_context is the low-level stack swap used by schedule() in sched.c.
; It saves callee-saved registers on the current stack, stores ESP, loads
; the next process's ESP and page directory, restores callee-saved regs, and
; returns — resuming wherever that process last called switch_context.
;
; process_initial_launch is the return address placed on the kernel stack of
; a never-run process by sched_build_initial_frame().  When switch_context
; "returns" into it, it performs the ISR-frame restore (pop segs, popa, iret)
; to enter ring 3 at the process's entry point.
;

section .text

; --------------------------------------------------------------------------
; void switch_context(uint32_t *old_esp_ptr,   ; [esp+4]  — may be NULL
;                     uint32_t  new_esp,        ; [esp+8]
;                     uint32_t  new_cr3);       ; [esp+12]
;
; Callee-saved regs (System V i386): EBX, ESI, EDI, EBP.
; After this function, the caller's EAX/ECX/EDX are clobbered (allowed by ABI).
; --------------------------------------------------------------------------
global switch_context
switch_context:
    ; Grab arguments while ESP still points to the caller's frame.
    mov eax, [esp+4]        ; old_esp_ptr (or NULL)
    mov ecx, [esp+8]        ; new_esp
    mov edx, [esp+12]       ; new_cr3

    ; Save callee-saved registers on the current (old) stack.
    push ebp
    push ebx
    push esi
    push edi

    ; Save current ESP into *old_esp_ptr (skip if NULL → zombie / no-save).
    test eax, eax
    jz .no_save
    mov [eax], esp
.no_save:

    ; ── Switch to the new process ──

    ; Load the new stack.
    mov esp, ecx

    ; Switch page directory.  Skipped if new_cr3 == current CR3 (same
    ; address space) to avoid a needless TLB flush.
    mov eax, cr3
    cmp eax, edx
    je .same_cr3
    mov cr3, edx
.same_cr3:

    ; Restore callee-saved registers from the new stack.
    pop edi
    pop esi
    pop ebx
    pop ebp

    ret                     ; "returns" into the new process's schedule() call


; --------------------------------------------------------------------------
; process_initial_launch
;
; Entry point for a process that has never run.  sched_build_initial_frame()
; places this address as the return address of the fake switch_context frame,
; directly above the synthesised ISR frame.  When switch_context does `ret`,
; it lands here with ESP pointing at the ISR frame's GS slot.
;
; We perform the same restore sequence as isr.asm / .restore:
;   pop gs, fs, es, ds → popa → skip vector+error_code → iret
; which launches the process into ring 3 at its ELF entry point.
; --------------------------------------------------------------------------
global process_initial_launch
global process_exec_launch
extern process_exec_cleanup
process_initial_launch:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8              ; discard vector + error_code
    iret

; --------------------------------------------------------------------------
; process_exec_launch
;
; Entry point for a successful in-place exec. After switch_context returns here
; on the new kernel stack, ESP points at two cleanup words followed by the ISR
; frame for the replacement image:
;   [esp+0] old_pd_phys
;   [esp+4] old_kstack_bottom
;
; Release the old address space and kernel stack, drop the cleanup words, then
; restore the ISR frame and iret into the replacement image.
; --------------------------------------------------------------------------
process_exec_launch:
    mov eax, [esp]
    mov edx, [esp+4]
    push edx
    push eax
    call process_exec_cleanup
    add esp, 8

    add esp, 8
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

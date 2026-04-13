; SPDX-License-Identifier: GPL-3.0-or-later
; isr.asm — exception, IRQ, and syscall entry stubs that build C-visible trap frames.

[bits 32]

; Exceptions without a CPU-pushed error code — push dummy 0 first
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

; Exceptions where the CPU already pushed an error code
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; Hardware IRQ stubs — vectors 32-47
%macro IRQ 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp irq_common
%endmacro

; CPU exception stubs (vectors 0-31)
; With error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQ stubs (IRQ 0-15 mapped to vectors 32-47)
IRQ  0, 32
IRQ  1, 33
IRQ  2, 34
IRQ  3, 35
IRQ  4, 36
IRQ  5, 37
IRQ  6, 38
IRQ  7, 39
IRQ  8, 40
IRQ  9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; Common ISR trampoline (CPU exceptions)
extern isr_handler
extern sched_signal_check
isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10        ; DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; Stack layout after push gs (low → high):
    ;   [esp+ 0] gs
    ;   [esp+ 4] fs
    ;   [esp+ 8] es
    ;   [esp+12] ds
    ;   [esp+16] edi
    ;   [esp+20] esi
    ;   [esp+24] ebp
    ;   [esp+28] esp_saved  (pusha value, unreliable)
    ;   [esp+32] ebx
    ;   [esp+36] edx
    ;   [esp+40] ecx
    ;   [esp+44] eax
    ;   [esp+48] vector
    ;   [esp+52] error_code
    ;   [esp+56] eip        ← CPU-pushed iret frame
    ;   [esp+60] cs
    ;   [esp+64] eflags
    ;   [esp+68] esp_user   (only on ring-3→0 transition)
    ;   [esp+72] ss_user
    ;
    ; Pass a single pointer to this frame to isr_handler.
    push esp
    call isr_handler
    add esp, 4
    ; Deliver any pending signal before returning to user space.
    push esp
    call sched_signal_check
    add esp, 4
    mov esp, eax
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8           ; discard vector and error_code pushed by stubs
    iret

; INT 0x80 syscall stub
; When this fires from ring 3, the CPU has already switched to the kernel
; stack (using TSS.ESP0) and pushed: SS_user, ESP_user, EFLAGS, CS_user, EIP_user.
global isr128
isr128:
    push dword 0    ; dummy error code
    push dword 128  ; vector number
    jmp syscall_common

; Syscall trampoline
; Stack layout after pusha + push ds/es/fs/gs (same as isr_common):
;   [esp+0]  gs
;   [esp+4]  fs
;   [esp+8]  es
;   [esp+12] ds
;   [esp+16] edi
;   [esp+20] esi
;   [esp+24] ebp
;   [esp+28] esp_saved  (pusha's esp value — unreliable, not used)
;   [esp+32] ebx        <- arg1 (syscall argument 1)
;   [esp+36] edx        <- arg3
;   [esp+40] ecx        <- arg2
;   [esp+44] eax        <- syscall number
;   [esp+48] vector (128)
;   [esp+52] error_code (0)
;   [esp+56] EIP_user
;   [esp+60] CS_user
;   [esp+64] EFLAGS
;   [esp+68] ESP_user   (present: CPL change from ring 3)
;   [esp+72] SS_user
;
; With the schedule-from-C model, context switches happen inside
; syscall_handler (via schedule()), not here.  The trampoline just calls
; the C handler, writes the return value, checks signals, and irets.

extern syscall_handler
extern sched_signal_check

syscall_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10        ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Read syscall arguments from the saved register frame
    mov eax, [esp+44]   ; syscall number (was EAX)
    mov ebx, [esp+32]   ; arg1 (was EBX)
    mov ecx, [esp+40]   ; arg2 (was ECX)
    mov edx, [esp+36]   ; arg3 (was EDX)
    mov esi, [esp+20]   ; arg4 (was ESI)
    mov edi, [esp+16]   ; arg5 (was EDI)

    ; Call syscall_handler(eax, ebx, ecx, edx, esi, edi) — cdecl: push right to left
    push edi
    push esi
    push edx
    push ecx
    push ebx
    push eax
    call syscall_handler
    add esp, 24
    mov [esp+44], eax   ; write return value into saved EAX slot → visible to user after iret

    ; Deliver any pending signal before returning to user space.
    push esp
    call sched_signal_check
    add esp, 4
    mov esp, eax                    ; update esp (no-op if unchanged)

.restore:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; discard vector and error_code
    iret

; Common IRQ trampoline (hardware interrupts)
; Frame layout identical to syscall_common above.
; g_irq_frame_esp is still recorded for the ring-0 check in
; schedule_if_needed() and for signal frame construction.

extern irq_dispatch
extern schedule_if_needed

global g_irq_frame_esp
g_irq_frame_esp: dd 0

irq_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Record the frame base (used by schedule_if_needed for ring check)
    mov [g_irq_frame_esp], esp

    push dword [esp+52]  ; error_code (dummy 0)
    push dword [esp+52]  ; vector
    call irq_dispatch
    add esp, 8

    ; If a context switch is pending (timer tick, keyboard wake, etc.),
    ; schedule_if_needed calls schedule() which switches stacks via
    ; switch_context.  When this process is rescheduled, schedule()
    ; returns here.
    call schedule_if_needed

    ; Deliver any pending signal before returning to user space.
    push esp
    call sched_signal_check
    add esp, 4
    mov esp, eax                    ; update esp (no-op if unchanged)

.irq_restore:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

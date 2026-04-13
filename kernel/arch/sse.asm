; SPDX-License-Identifier: GPL-3.0-or-later
; sse.asm — assembly helpers that enable SSE and save a clean initial FPU state image.

[bits 32]

global sse_init
global sse_initial_fpu_state

section .bss
align 16
sse_initial_fpu_state: resb 512

section .text

; void sse_init(void)
;
; Enables the x87 FPU and SSE2:
;   CR0: clear EM (bit 2) so FP/SSE instructions don't fault,
;        set   MP (bit 1) so #NM fires when TS=1 (lazy FPU switch later).
;   CR4: set OSFXSR (bit 9)    — OS supports FXSAVE/FXRSTOR
;        set OSXMMEXCPT (bit 10) — OS handles #XF SSE exceptions
;
; After enabling the hardware, saves a clean FXSAVE image into
; sse_initial_fpu_state so process_create can copy it as the initial
; FPU state for every new process.
sse_init:
    ; --- CR0 ---
    mov eax, cr0
    and eax, ~(1 << 2)          ; clear EM: no FPU emulation
    or  eax,  (1 << 1)          ; set MP: monitor coprocessor
    mov cr0, eax

    ; --- CR4 ---
    mov eax, cr4
    or  eax, (1 << 9) | (1 << 10)  ; OSFXSR | OSXMMEXCPT
    mov cr4, eax

    ; Initialize x87 to its default state (FCW=0x037F, FSW=0, FTW=0xFF)
    fninit

    ; Capture the clean FPU+SSE state (MXCSR=0x1F80, all XMM=0) as a
    ; template that process_create copies into every new process_t.
    fxsave [sse_initial_fpu_state]

    ret

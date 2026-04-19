; SPDX-License-Identifier: GPL-3.0-or-later
; Startup object used by the TinyCC compatibility sysroot.
;
; This is the C-only subset of crt0.asm: it initializes environ, calls
; main(argc, argv, envp), then exits through the Drunix syscall wrapper.
; It intentionally avoids constructor-table linker symbols because TinyCC's
; built-in linker does not consume the Drunix user.ld script yet.

bits 32

global _start
extern main
extern sys_exit
extern environ

section .text
_start:
    mov   eax, [esp]              ; argc
    lea   ebx, [esp + 4]          ; argv
    lea   ecx, [ebx + eax * 4 + 4] ; envp
    mov   [environ], ecx
    push  ecx
    push  ebx
    push  eax
    call  main
    add   esp, 12
    push  eax
    call  sys_exit
.hang:
    hlt
    jmp   .hang

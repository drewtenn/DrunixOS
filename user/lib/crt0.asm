; SPDX-License-Identifier: GPL-3.0-or-later
; crt0.asm — C runtime startup for ring-3 user programs.
;
; Provides _start, the ELF entry point declared in user.ld.
;
; The kernel's process_create() pre-populates the user stack with a
; System V i386 argv frame before iret'ing to ring 3.  On entry, ESP
; points at argc, with the char** argv pointer one word above it:
;
;     [esp + 0] = argc (int)
;     [esp + 4] = argv (char**)    — points further up into this same page
;     [esp + 8] = envp (char**)    — NULL-terminated environment vector
;
; crt0 pops all three, stores envp in the libc global `environ`, runs C/C++
; constructors, pushes them in cdecl order, calls main(argc, argv, envp), runs
; C/C++ destructors, then feeds the return value to sys_exit().  Programs with
; a two-argument main simply ignore the third stack argument under cdecl.

bits 32

global _start
extern main
extern sys_exit
extern environ
extern __drunix_run_constructors
extern __drunix_run_destructors

section .text
_start:
    pop   eax        ; argc
    pop   ebx        ; argv (char **)
    pop   ecx        ; envp (char **)
    mov   [environ], ecx
    push  eax
    push  ebx
    push  ecx
    call  __drunix_run_constructors
    pop   ecx
    pop   ebx
    pop   eax
    push  ecx        ; main's third arg
    push  ebx        ; main's second arg
    push  eax        ; main's first arg
    call  main
    add   esp, 12    ; drop the three args we pushed
    push  eax
    call  __drunix_run_destructors
    pop   eax
    push  eax        ; exit code = return value of main
    call  sys_exit
.hang:
    hlt
    jmp   .hang      ; unreachable — sys_exit never returns

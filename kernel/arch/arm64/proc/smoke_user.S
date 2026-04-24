.section .text
.global _start

_start:
    mov x0, #1
    adrp x1, msg_entered
    add x1, x1, :lo12:msg_entered
    mov x2, #26
    mov x8, #64
    svc #0

    mov x0, #1
    adrp x1, msg_syscall
    add x1, x1, :lo12:msg_syscall
    mov x2, #29
    mov x8, #64
    svc #0

    mov x0, #1
    adrp x1, msg_pass
    add x1, x1, :lo12:msg_pass
    mov x2, #23
    mov x8, #64
    svc #0

    mov x0, #0
    mov x8, #93
    svc #0

1:
    wfe
    b 1b

.section .rodata
msg_entered:
    .ascii "ARM64 user smoke: entered\n"
msg_syscall:
    .ascii "ARM64 user smoke: syscall ok\n"
msg_pass:
    .ascii "ARM64 user smoke: pass\n"

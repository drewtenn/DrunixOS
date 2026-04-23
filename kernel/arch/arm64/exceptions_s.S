.extern g_arm64_exception_frame

.macro SAVE_GPRS
    sub sp, sp, #288
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x9, sp_el0
    str x9, [sp, #248]
    mrs x9, elr_el1
    str x9, [sp, #256]
    mrs x9, spsr_el1
    str x9, [sp, #264]
    mrs x9, esr_el1
    str x9, [sp, #272]
    mrs x9, far_el1
    str x9, [sp, #280]
.endm

.macro RESTORE_GPRS
    ldr x9, [sp, #248]
    msr sp_el0, x9
    ldr x9, [sp, #256]
    msr elr_el1, x9
    ldr x9, [sp, #264]
    msr spsr_el1, x9
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #288
.endm

.macro SET_CURRENT_FRAME
    adrp x9, g_arm64_exception_frame
    add x9, x9, :lo12:g_arm64_exception_frame
    mov x10, sp
    str x10, [x9]
.endm

.macro CLEAR_CURRENT_FRAME
    adrp x9, g_arm64_exception_frame
    add x9, x9, :lo12:g_arm64_exception_frame
    str xzr, [x9]
.endm

.macro VENTRY label
    .balign 0x80
    b \label
.endm

.macro HANDLE label, cfunc
\label:
    SAVE_GPRS
    SET_CURRENT_FRAME
    mov x0, sp
    bl \cfunc
    CLEAR_CURRENT_FRAME
    RESTORE_GPRS
    eret
.endm

.section ".text.vectors", "ax"
.balign 0x800

.global vectors_el1
vectors_el1:
    VENTRY sync_sp0
    VENTRY irq_sp0
    VENTRY fiq_sp0
    VENTRY error_sp0

    VENTRY sync_spx
    VENTRY irq_spx
    VENTRY fiq_spx
    VENTRY error_spx

    VENTRY sync_a64
    VENTRY irq_a64
    VENTRY fiq_a64
    VENTRY error_a64

    VENTRY sync_a32
    VENTRY irq_a32
    VENTRY fiq_a32
    VENTRY error_a32

.text

HANDLE sync_sp0, arm64_sync_handler
HANDLE irq_sp0, arm64_irq_handler
HANDLE fiq_sp0, arm64_fiq_handler
HANDLE error_sp0, arm64_serror_handler

HANDLE sync_spx, arm64_sync_handler
HANDLE irq_spx, arm64_irq_handler
HANDLE fiq_spx, arm64_fiq_handler
HANDLE error_spx, arm64_serror_handler

HANDLE sync_a64, arm64_sync_handler
HANDLE irq_a64, arm64_irq_handler
HANDLE fiq_a64, arm64_fiq_handler
HANDLE error_a64, arm64_serror_handler

HANDLE sync_a32, arm64_sync_handler
HANDLE irq_a32, arm64_irq_handler
HANDLE fiq_a32, arm64_fiq_handler
HANDLE error_a32, arm64_serror_handler

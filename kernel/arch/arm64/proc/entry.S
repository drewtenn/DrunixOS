.equ ARM64_KERNEL_CONTEXT_SIZE, 96

.text

.global arm64_switch_context
arm64_switch_context:
    sub sp, sp, #ARM64_KERNEL_CONTEXT_SIZE
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]

    cbz x0, .Lswitch_load
    mov x2, sp
    str x2, [x0]

.Lswitch_load:
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #ARM64_KERNEL_CONTEXT_SIZE
    ret

.global arm64_process_first_resume
arm64_process_first_resume:
    mov x0, x19
    b arm64_enter_user_frame

.global arm64_enter_user_frame
arm64_enter_user_frame:
    ldr x1, [x0, #248]
    msr sp_el0, x1
    ldr x1, [x0, #256]
    msr elr_el1, x1
    ldr x1, [x0, #264]
    msr spsr_el1, x1
    isb

    ldp x1, x2, [x0, #8]
    ldp x3, x4, [x0, #24]
    ldp x5, x6, [x0, #40]
    ldp x7, x8, [x0, #56]
    ldp x9, x10, [x0, #72]
    ldp x11, x12, [x0, #88]
    ldp x13, x14, [x0, #104]
    ldp x15, x16, [x0, #120]
    ldp x17, x18, [x0, #136]
    ldp x19, x20, [x0, #152]
    ldp x21, x22, [x0, #168]
    ldp x23, x24, [x0, #184]
    ldp x25, x26, [x0, #200]
    ldp x27, x28, [x0, #216]
    ldp x29, x30, [x0, #232]
    ldr x0, [x0, #0]
    eret

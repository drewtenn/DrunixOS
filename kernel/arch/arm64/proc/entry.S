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

.global arm64_fpu_save_state
arm64_fpu_save_state:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    ret

.global arm64_fpu_restore_state
arm64_fpu_restore_state:
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ret

.global arm64_process_first_resume
arm64_process_first_resume:
    mov x0, x19
    b arm64_enter_user_frame

.global arm64_process_exec_resume
.extern process_exec_cleanup
arm64_process_exec_resume:
    mov x0, x20
    mov x1, x21
    bl process_exec_cleanup
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

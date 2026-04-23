.text

.global arm64_enter_el0
arm64_enter_el0:
    msr sp_el0, x1
    msr elr_el1, x0
    msr spsr_el1, xzr
    isb
    eret

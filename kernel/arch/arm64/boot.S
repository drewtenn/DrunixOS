.section ".text.boot", "ax"

.global _start

_start:
    /* x0 holds the FDT/DTB pointer per the Linux ARM64 boot protocol.
     * Preserve it through the EL transitions and the BSS zero loop into
     * x19 (callee-saved by AAPCS64); publish to g_fdt_blob_phys after
     * BSS is zeroed so the C runtime can consume it. */
    mov x19, x0

    mrs x1, mpidr_el1
    and x1, x1, #0xFF
    cbnz x1, .Lpark

    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.eq .Lfrom_el2
    cmp x0, #3
    b.eq .Lfrom_el3
    cmp x0, #1
    b.eq .Lin_el1
    b .Lpark

.Lfrom_el3:
    mov x0, #0x0501
    msr scr_el3, x0

    mov x0, #0x3c9
    msr spsr_el3, x0
    adr x0, .Lfrom_el2
    msr elr_el3, x0
    eret

.Lfrom_el2:
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    msr cptr_el2, xzr

    mov x0, #0x3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr

    mov x0, #0x0800
    movk x0, #0x30d0, lsl #16
    msr sctlr_el1, x0

    mov x0, #0x3c5
    msr spsr_el2, x0
    adr x0, .Lin_el1
    msr elr_el2, x0
    eret

.Lin_el1:
    mrs x0, sctlr_el1
    bic x0, x0, #(1 << 1)
    bic x0, x0, #(1 << 3)
    bic x0, x0, #(1 << 4)
    msr sctlr_el1, x0
    isb

    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    adr x0, _start
    mov sp, x0

    ldr x0, =__bss_start
    ldr x1, =__bss_end
.Lbss_zero:
    cmp x0, x1
    b.hs .Lbss_done
    str xzr, [x0], #8
    b .Lbss_zero

.Lbss_done:
    /* BSS is zero; safe to publish the saved FDT pointer. */
    ldr x0, =g_fdt_blob_phys
    str x19, [x0]

    bl arm64_start_kernel

.Lpark:
    wfe
    b .Lpark

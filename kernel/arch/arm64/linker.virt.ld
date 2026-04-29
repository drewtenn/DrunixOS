OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(0x5); /* R + X */
    rodata PT_LOAD FLAGS(0x4); /* R */
    data   PT_LOAD FLAGS(0x6); /* R + W */
}

/*
 * QEMU's `-kernel` for `-M virt` loads at 0x40080000 by Linux ARM64
 * kernel-image convention (RAM base 0x40000000 + 0x80000 offset).
 * QEMU honors the ELF PT_LOAD addresses, so this layout is what gets
 * placed in physical RAM.
 */
SECTIONS {
    . = 0x40080000;
    _kernel_start = .;

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
    } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
        *(.got*)
    } :data

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    } :data
    . = ALIGN(16);
    __bss_end = .;
    _kernel_end = .;
}

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(0x5); /* R + X */
    rodata PT_LOAD FLAGS(0x4); /* R */
    data   PT_LOAD FLAGS(0x6); /* R + W */
}

SECTIONS {
    . = 0x80000;
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

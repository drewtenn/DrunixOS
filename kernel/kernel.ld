ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(0x5); /* R + X */
    rodata PT_LOAD FLAGS(0x4); /* R */
    data   PT_LOAD FLAGS(0x6); /* R + W */
}

SECTIONS {
    . = 0x100000;

    _kernel_start = .;

    .multiboot : { *(.multiboot) } :text
    .text      : { *(.text*)     } :text

    . = ALIGN(0x1000);
    .rodata    : { *(.rodata*)   } :rodata

    . = ALIGN(0x1000);
    .data      : { *(.data*)     } :data
    .bss       : {
        *(.bss)
        *(COMMON)
    } :data

    _kernel_end = .;
}

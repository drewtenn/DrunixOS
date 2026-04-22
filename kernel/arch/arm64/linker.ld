OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    . = 0x80000;
    _kernel_start = .;

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    }

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
    }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;
    _kernel_end = .;
}

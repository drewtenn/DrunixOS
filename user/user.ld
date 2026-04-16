/*
 * user.ld — linker script for ring-3 user programs.
 *
 * Loads the binary at virtual address 0x400000 (4 MB), which is well above
 * the kernel's 0–2 MB range and safely below the 3 GB user/kernel split.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .init_array : {
        __init_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP (*(.init_array))
        __init_array_end = .;
    }

    .ctors : {
        __ctors_start = .;
        KEEP (*(.ctors))
        KEEP (*(SORT_BY_NAME(.ctors.*)))
        __ctors_end = .;
    }

    .fini_array : {
        __fini_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP (*(.fini_array))
        __fini_array_end = .;
    }

    .dtors : {
        __dtors_start = .;
        KEEP (*(.dtors))
        KEEP (*(SORT_BY_NAME(.dtors.*)))
        __dtors_end = .;
    }

    .eh_frame : {
        *(.eh_frame)
        *(.eh_frame.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
    }
}

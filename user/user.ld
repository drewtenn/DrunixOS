/*
 * user.ld — linker script for ring-3 user programs.
 *
 * Loads the binary at virtual address 0x400000 (4 MB), which is well above
 * the kernel's 0–2 MB range and safely below the 3 GB user/kernel split.
 */

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(0x5); /* R + X */
    rodata PT_LOAD FLAGS(0x4); /* R */
    data   PT_LOAD FLAGS(0x6); /* R + W */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text)
        *(.text.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :rodata

    .eh_frame : {
        *(.eh_frame)
        *(.eh_frame.*)
    } :rodata

    . = ALIGN(4096);
    .init_array : {
        __init_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP (*(.init_array))
        __init_array_end = .;
    } :data

    .ctors : {
        __ctors_start = .;
        KEEP (*(.ctors))
        KEEP (*(SORT_BY_NAME(.ctors.*)))
        __ctors_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP (*(.fini_array))
        __fini_array_end = .;
    } :data

    .dtors : {
        __dtors_start = .;
        KEEP (*(.dtors))
        KEEP (*(SORT_BY_NAME(.dtors.*)))
        __dtors_end = .;
    } :data

    .data : {
        *(.data)
        *(.data.*)
    } :data

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
    } :data
}

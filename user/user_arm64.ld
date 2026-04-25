/*
 * ARM64 user program linker script.
 *
 * The ARM64 loader currently accepts executable segments in the
 * 0x02000000-0x02800000 user image window.
 */

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(0x5);
    rodata PT_LOAD FLAGS(0x4);
    data   PT_LOAD FLAGS(0x6);
}

SECTIONS
{
    . = 0x02000000;

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

/*
 * user.ld — linker script for ring-3 user programs.
 *
 * Loads the binary at virtual address 0x400000 (4 MB), which is well above
 * the kernel's 0–2 MB range and safely below the 3 GB user/kernel split.
 */

ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text   : { *(.text)   }
    .rodata : { *(.rodata) }
    .data   : { *(.data)   }
    .bss    : {
        *(.bss)
        *(COMMON)
        . = ALIGN(4096);   /* align BSS end to page boundary */
    }
}

# Linux Reference Policy

When planning a design or debugging a problem, check how Linux solves the same problem. Linux has already faced the same low-level OS concerns this project will encounter, including memory layout, driver initialization order, struct alignment with hardware specs, and race conditions.

Useful references:

- Memory layout / PMM: `mm/page_alloc.c`, `mm/memblock.c`
- Boot / Multiboot struct parsing: `arch/x86/boot/`, `arch/x86/kernel/setup.c`
- ATA / block drivers: `drivers/ata/`
- ELF loading: `fs/binfmt_elf.c`
- Syscall dispatch: `arch/x86/entry/entry_32.S`, `kernel/sys.c`
- IDT / exceptions: `arch/x86/kernel/idt.c`, `arch/x86/kernel/traps.c`

When the answer is "Linux does it this way", document that in the relevant `docs/` chapter so the reasoning is preserved.

If the Linux approach depends on functionality this project does not yet have, stop and ask whether that missing functionality should be implemented first. Do not silently design a workaround or simplify the approach without surfacing the dependency gap.

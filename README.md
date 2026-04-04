# osdev

A bare-metal x86 operating system built from scratch. Boots in 16-bit real mode, loads a kernel from disk, transitions to 32-bit protected mode, and runs a minimal C kernel with a VGA text-mode driver.

## Architecture

```
boot_sect.asm    →  bootsector: loads kernel from disk, switches to protected mode
kernel-entry.asm →  32-bit entry point, calls start_kernel()
kernel.c         →  VGA driver, print_string, cursor management
```

### Boot sequence

1. BIOS loads the 512-byte bootsector at `0x7C00`
2. Bootsector prints a real-mode message, loads the kernel (2 sectors) into memory at `0x1000` via BIOS disk routines
3. Switches to 32-bit protected mode via a GDT
4. Jumps to `kernel-entry.asm`, which calls `start_kernel()` in C

### Kernel (kernel.c)

- **VGA text driver** — writes directly to video memory at `0xB8000` (80×25 white-on-black)
- **print_string** — prints a null-terminated string, handles `\n` and auto-scrolls
- **Cursor control** — reads/writes hardware cursor position via VGA I/O ports (`0x3D4`/`0x3D5`)
- **Scrolling** — shifts all rows up by one when the screen is full

## Dependencies

- `nasm` — assembler
- `x86_64-elf-gcc` — cross-compiler targeting i386 freestanding
- `x86_64-elf-ld` — linker
- `qemu-system-i386` — emulator
- `i386-elf-gdb` — debugger (optional, for `make debug`)

On macOS these can be installed via Homebrew:

```sh
brew install nasm qemu x86_64-elf-gcc
```

## Build & Run

```sh
make        # build and run in QEMU
make debug  # launch QEMU with GDB server on :1234, attach GDB with kernel symbols
make clean  # remove build artifacts
```

## Status

- [x] 16-bit real mode boot
- [x] Disk load (BIOS INT 13h)
- [x] GDT and protected mode switch
- [x] 32-bit C kernel entry
- [x] VGA text output with scrolling and cursor control
- [ ] Keyboard input (ISR / IDT not yet implemented)
- [ ] Memory management
- [ ] Filesystem

# Drunix

## Project Summary

Drunix is a 32-bit x86 hobby operating system that boots through GRUB2 with the Multiboot1 protocol and runs a freestanding C kernel. The kernel provides protected-mode interrupt handling, paging, a physical and heap allocator, ATA disk I/O, a DUFS filesystem, a mount-tree VFS with synthetic `/dev` and `/proc` namespaces, preemptive scheduling built around generic wait queues, signals, a TTY subsystem with job control, an ELF user-program loader, and per-process virtual-memory bookkeeping for demand-paged heaps, grow-down stacks, copy-on-write fork, and anonymous `mmap` regions. The disk image includes a small userland with a shell and basic utilities.

## Dependencies

Required:

- `make`
- `python3`
- `nasm`
- `qemu-system-i386`
- `x86_64-elf-gcc`
- `x86_64-elf-ld`
- `i686-elf-grub-mkrescue`
- `xorriso`

Optional:

- `i386-elf-gdb` for `make debug`
- `pandoc` for `make epub`, `make pdf`, and `make docs`
- `cairosvg` (Python package) or `rsvg-convert` for `make epub`, `make pdf`, and `make docs` — converts SVG diagrams to PNG
- `calibre` (provides `ebook-convert`) for `make pdf` and `make docs` — renders the PDF from the built EPUB

## Install Dependencies

### macOS

Install Homebrew first, then:

```sh
brew install make nasm python qemu x86_64-elf-gcc i686-elf-grub xorriso
brew install i386-elf-gdb pandoc
brew install --cask calibre
pip3 install cairosvg
# or: brew install librsvg
```

### Windows

The simplest supported setup is WSL2 with Ubuntu. Inside the WSL shell:

```sh
sudo apt update
sudo apt install -y build-essential python3 nasm qemu-system-x86 xorriso grub-pc-bin mtools pandoc calibre
pip3 install cairosvg
```

You still need an `x86_64-elf` cross toolchain and `i386-elf-gdb` on your `PATH`. If your package set does not provide them directly, build and install the usual OSDev cross toolchain, then verify these commands exist:

```sh
x86_64-elf-gcc --version
x86_64-elf-ld --version
i386-elf-gdb --version
i686-elf-grub-mkrescue --version
```

If your distro provides `grub-mkrescue` instead of `i686-elf-grub-mkrescue`, create a compatibility symlink or wrapper so the command name used by the Makefile exists.

### Linux

Package names vary by distro, but you need the same toolchain listed above.

Ubuntu / Debian:

```sh
sudo apt update
sudo apt install -y build-essential python3 nasm qemu-system-x86 xorriso grub-pc-bin mtools pandoc calibre
pip3 install cairosvg
```

Fedora:

```sh
sudo dnf install -y make python3 nasm qemu-system-i386 xorriso grub2-tools-extra mtools pandoc calibre
pip3 install cairosvg
```

Arch:

```sh
sudo pacman -S --needed make python nasm qemu-desktop xorriso grub mtools pandoc calibre
pip3 install cairosvg
```

As on Windows, make sure the `x86_64-elf` cross compiler/linker and optional `i386-elf-gdb` are installed and visible on `PATH`.

If your distro installs `grub-mkrescue` but not `i686-elf-grub-mkrescue`, add a symlink or wrapper with the `i686-elf-grub-mkrescue` name because that is what this repo's Makefile invokes.

## Build And Run

Build the kernel, ISO, disk image, and launch QEMU:

```sh
make
```

Useful targets:

- `make run` rebuilds the kernel and ISO, then launches QEMU without rebuilding `disk.img`
- `make disk` rebuilds `disk.img`
- `make kernel` rebuilds `kernel.elf` and `os.iso`
- `make test` boots with the in-kernel unit tests enabled
- `make KTEST=1 run` boots with the in-kernel unit tests enabled
- `make rebuild` wipes outputs, rebuilds everything, and boots from scratch
- `make epub` builds the EPUB edition
- `make pdf` builds the PDF book by converting the EPUB with `ebook-convert`
- `make docs` builds both the EPUB and the PDF
- `make clean` removes build outputs

## Debugging

Start QEMU paused with a GDB stub and attach GDB:

```sh
make debug
```

Other useful debug flows:

- `make run-debugcon-stdio` streams QEMU debug console output to your terminal
- `make KLOG_TO_DEBUGCON=1 run` mirrors ordinary `klog()` output to QEMU debugcon
- `make test-double-fault` boots a special image that verifies the dedicated double-fault path

Runtime logs:

- `serial.log` captures COM1 output
- `debugcon.log` captures QEMU debug console output on port `0xE9`

Fatal kernel faults write diagnostics to serial and debugcon so they remain visible even when VGA output is no longer reliable.

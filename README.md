# Drunix

## Project Summary

Drunix is a 32-bit x86 hobby operating system that boots through GRUB2 with the Multiboot1 protocol and runs a freestanding C kernel. The kernel provides protected-mode interrupt handling, paging, a physical and heap allocator, ATA disk I/O, a DUFS filesystem, a mount-tree VFS with synthetic `/dev` and `/proc` namespaces, preemptive scheduling built around generic wait queues, signals, a TTY subsystem with job control, an ELF user-program loader, and per-process virtual-memory bookkeeping for demand-paged heaps, grow-down stacks, copy-on-write fork, and anonymous `mmap` regions. User programs can be written in C or in a small freestanding C++ subset backed by the repo-owned user runtime.

The normal boot path asks GRUB for a 1024x768x32 linear framebuffer and starts a simple GUI desktop. The boot shell is opened as the main desktop app inside that GUI shell, with keyboard input, PS/2 mouse pointer support, taskbar/menu launching, framebuffer text rendering, double-buffered flicker-free compositing with an overlay mouse cursor, and a VGA text-mode fallback when a suitable framebuffer is unavailable. The disk image includes a small userland with a C shell and C++ utility programs.

<a href="docs/drunix-desktop.png">
  <img src="docs/drunix-desktop.png" alt="Drunix desktop running in QEMU with Files, Processes, Help, and Shell windows open">
</a>

## Dependencies

Required:

- `make`
- `python3`
- `nasm`
- `qemu-system-i386`
- `x86_64-elf-gcc`
- `x86_64-elf-g++`
- `x86_64-elf-ld`
- `i686-elf-grub-mkrescue`
- `xorriso`

Optional:

- `i386-elf-gdb` for `make debug`
- `pandoc` for `make epub`, `make pdf`, and `make docs`
- `typst` for `make pdf` and `make docs`
- `cairosvg` (Python package) or `rsvg-convert` for `make epub` and `make docs` — converts SVG diagrams to PNG
- `zip`, `unzip`, and `perl` for EPUB packaging

## Install Dependencies

### macOS

Install Homebrew first, then:

```sh
brew install make nasm python qemu x86_64-elf-gcc i686-elf-grub xorriso
brew install i386-elf-gdb pandoc typst
pip3 install cairosvg
# or: brew install librsvg
```
Verify the compiler tools you need are on `PATH`:

```sh
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version
```

### Windows

The simplest supported setup is WSL2 with Ubuntu. Inside the WSL shell:

```sh
sudo apt update
sudo apt install -y build-essential python3 nasm qemu-system-x86 xorriso grub-pc-bin mtools pandoc typst zip unzip perl
pip3 install cairosvg
```

You still need an `x86_64-elf` cross toolchain and `i386-elf-gdb` on your `PATH`. If your package set does not provide them directly, build and install the usual OSDev cross toolchain, then verify these commands exist:

```sh
x86_64-elf-gcc --version
x86_64-elf-g++ --version
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
sudo apt install -y build-essential python3 nasm qemu-system-x86 xorriso grub-pc-bin mtools pandoc typst zip unzip perl
pip3 install cairosvg
```

Fedora:

```sh
sudo dnf install -y make python3 nasm qemu-system-i386 xorriso grub2-tools-extra mtools pandoc typst zip unzip perl
pip3 install cairosvg
```

Arch:

```sh
sudo pacman -S --needed make python nasm qemu-desktop xorriso grub mtools pandoc typst zip unzip perl
pip3 install cairosvg
```

As on Windows, make sure the `x86_64-elf` cross compiler/linker and optional `i386-elf-gdb` are installed and visible on `PATH`.

Verify the compiler tools you need are on `PATH`:

```sh
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version
```

If your distro installs `grub-mkrescue` but not `i686-elf-grub-mkrescue`, add a symlink or wrapper with the `i686-elf-grub-mkrescue` name because that is what this repo's Makefile invokes.

## Build And Run

For a clean first boot, build the kernel, ISO, disk image, and launch QEMU:

```sh
make run-fresh
```

On a normal QEMU boot, Drunix opens the shell inside the framebuffer desktop. If the bootloader does not provide a usable 32-bit RGB framebuffer, the kernel falls back to the legacy VGA text presentation path.

Useful targets:

- `make all` rebuilds the kernel and ISO as needed, then launches QEMU with the existing `disk.img`
- `make run` rebuilds the kernel and ISO as needed, then launches QEMU without rebuilding `disk.img`
- `make run-fresh` rebuilds `disk.img`, then launches QEMU
- `make disk` rebuilds `disk.img`
- `make kernel` rebuilds `kernel.elf` and `os.iso`
- `make test` boots with the in-kernel unit tests enabled
- `make KTEST=1 run` boots with the in-kernel unit tests enabled
- `make test-all` runs the in-kernel unit tests and halt-inducing tests
- `make rebuild` wipes build outputs, rebuilds the kernel and disk image, and boots from scratch
- `make epub` builds the EPUB edition
- `make pdf` builds the PDF book from the Markdown sources with Pandoc and Typst
- `make docs` builds both the EPUB and the PDF
- `make clean` removes build outputs

### Build Options

Mouse cursor speed in the framebuffer desktop is controlled at build time:

```sh
make MOUSE_SPEED=6 os.iso
```

`MOUSE_SPEED` defaults to `4`. Values below `1` use `1`; values above `16` use
`16`. The option affects framebuffer mouse motion only. VGA text fallback
pointer motion remains unscaled, at one pixel per raw mouse unit before cell
coordinates are derived.

### Userland C++ Support

User programs can be written in C or in a freestanding C++ subset. C programs
continue to compile with `x86_64-elf-gcc`; C++ programs compile with
`x86_64-elf-g++` and link against the repo-owned user runtime in `user/lib`.

The current C++ userland supports global constructors and destructors,
classes, virtual dispatch, `new`, `delete`, `new[]`, and `delete[]`.
Allocation uses the existing `malloc` and `free` implementation backed by
`SYS_BRK`.

Exceptions, RTTI, `libstdc++`, and `libsupc++` are not part of the current
runtime. Code that depends on those features should fail at compile or link
time instead of pulling in hosted runtime libraries implicitly.

The smoke binary is `/bin/cpphello`, built from `user/cpphello.cpp`.
All packaged non-shell utilities are also C++ sources. The shell and the
runtime library remain C for now.
The book-level walkthrough is Chapter 30, `docs/ch30-cpp-userland.md`.

## Debugging

Start QEMU paused with a GDB stub and attach GDB:

```sh
make debug
```

Other useful debug flows:

- `make run-stdio` streams QEMU debug console output to your terminal
- `make KLOG_TO_DEBUGCON=1 run` mirrors ordinary `klog()` output to QEMU debugcon
- `make test-halt` boots a special image that verifies the dedicated double-fault path

Runtime logs:

- `serial.log` captures COM1 output
- `debugcon.log` captures QEMU debug console output on port `0xE9`

Fatal kernel faults write diagnostics to serial and debugcon so they remain visible even when the framebuffer or VGA display path is no longer reliable.

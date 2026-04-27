# Drunix

## Project Summary

Drunix is a hobby operating system with a feature-rich 32-bit x86 mainline and a newer AArch64 bring-up path. The mainline x86 kernel boots through GRUB2 with the Multiboot1 protocol and provides protected-mode interrupt handling, paging, a physical and heap allocator, ATA disk I/O, a DUFS filesystem, a mount-tree VFS with synthetic `/dev`, `/proc`, and `/sys` namespaces, preemptive scheduling built around generic wait queues, Linux-style `clone` user threads, ref-counted process resources for address spaces, file descriptors, filesystem state, and signal actions, a TTY subsystem with job control, an ELF user-program loader, and per-process virtual-memory bookkeeping for demand-paged heaps, grow-down stacks, copy-on-write fork, and anonymous `mmap` regions. User programs can be written in C or in a small freestanding C++ subset backed by the repo-owned user runtime.

The x86 desktop build asks GRUB for a 1024x768x32 linear framebuffer and starts a simple GUI desktop. The boot shell is opened as the main desktop app inside that GUI shell, with keyboard input, PS/2 mouse pointer support, a dark wallpapered desktop, icon-only taskbar, clock, framebuffer text rendering, scene-buffered compositing with an overlay mouse cursor drawn from a shared sprite mask, and a VGA text-mode fallback when a suitable framebuffer is unavailable. The disk image includes a small mixed-language userland: the shell and `chello` exercise the C runtime path, while the utility programs exercise the C++ runtime path.

The AArch64 path now boots the Raspberry Pi 3 / `qemu-system-aarch64` target through EL1 setup, mini-UART, exception vectors, timer IRQs, MMU and heap setup, an initramfs-backed user program, and the same graphical-by-default workflow shape as x86. `make ARCH=arm64 run` opens the QEMU display and mirrors the ARM64 console into a framebuffer text terminal.

[![Drunix desktop running in QEMU with the shell window, curved blue wallpaper, icon dock, and clock](docs/drunix-desktop.png)](docs/drunix-desktop.png)

## Status

- x86 remains the mainline Drunix target: desktop boot, ext3 and DUFS disk images, process and signal support, Linux ABI smoke coverage, copy-on-write fork, demand paging, and the freestanding C/C++ userland are all part of the regular workflow.
- AArch64 is now in-tree as a smaller but usable second target: `ARCH=arm64` builds and boots on the QEMU `raspi3b` machine, initializes the BCM2835 mini-UART and framebuffer console, brings up MMU-backed kernel services, loads an initramfs user program, and runs the console prompt. Keyboard input and the desktop are tracked as later ARM64 phases.

## Dependencies

Required:

- `make`
- `python3`
- `nasm`
- `qemu-system-i386`
- `x86_64-elf-gcc`
- `x86_64-elf-g++`
- `x86_64-elf-ld`
- `i486-linux-musl-gcc`
- `i686-elf-grub-mkrescue`
- `xorriso`

Optional:

- `i386-elf-gdb` for `make debug`
- `clang-format`, `clang-tidy`, `cppcheck`, and `sparse` for `make scan`,
  `make check`, `make format-check`, `make clang-tidy-include-check`,
  `make cppcheck`, and `make sparse-check`
- `aarch64-elf-gcc`, `aarch64-elf-g++`, `aarch64-elf-ld`, and
  `aarch64-elf-objcopy` for `make ARCH=arm64 run` and
  `make ARCH=arm64 check`
- `qemu-system-aarch64` for the AArch64 / Raspberry Pi 3 bring-up path
- `pandoc` for `make epub`, `make pdf`, and `make docs`
- `typst` for `make pdf` and `make docs`
- `rsvg-convert` from `librsvg` for `make epub` and `make docs`; converts SVG diagrams to PNG
- `zip`, `unzip`, and `perl` for `make epub` and `make docs`; used to repackage the EPUB after post-processing

## Install Dependencies

### macOS

Install Homebrew first, then:

```sh
brew install make nasm python qemu x86_64-elf-gcc i686-elf-grub xorriso
brew install i386-elf-gdb pandoc typst
brew install clang-format llvm cppcheck sparse
brew install librsvg
```
Verify the compiler tools you need are on `PATH`:

```sh
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version
i486-linux-musl-gcc --version
aarch64-elf-gcc --version
```

Homebrew's `clang-tidy` is provided by LLVM. If `clang-tidy` is not on `PATH`
after installing `llvm`, either add LLVM's bin directory to `PATH` or pass it to
make explicitly:

```sh
make CLANG_TIDY="$(brew --prefix llvm)/bin/clang-tidy" clang-tidy-include-check
```

The `x86_64-elf-*` tools build the freestanding Drunix kernel and native
Drunix user programs. The `i486-linux-musl-gcc` tool builds static Linux i386
ABI probes such as `linuxprobe` and generated BusyBox binaries. One portable
way to install that musl toolchain on macOS is the Homebrew musl-cross package
with the i486 target enabled:

```sh
brew tap filosottile/musl-cross
brew install musl-cross --with-i486
i486-linux-musl-gcc --version
```

### Windows

The simplest supported setup is WSL2 with Ubuntu. Inside the WSL shell:

```sh
sudo apt update
sudo apt install -y build-essential python3 curl nasm qemu-system-x86 xorriso grub-pc-bin mtools pandoc typst zip unzip perl librsvg2-bin clang-format clang-tidy cppcheck sparse
```

You still need an `x86_64-elf` cross toolchain and `i386-elf-gdb` on your `PATH`. If your package set does not provide them directly, build and install the usual OSDev cross toolchain, then verify these commands exist:

```sh
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version
i486-linux-musl-gcc --version
i386-elf-gdb --version
i686-elf-grub-mkrescue --version
```

If your package set does not include `i486-linux-musl-gcc`, install a musl
cross toolchain and put its `bin` directory on `PATH`:

```sh
mkdir -p "$HOME/toolchains"
curl -L -o /tmp/i486-linux-musl-cross.tgz https://musl.cc/i486-linux-musl-cross.tgz
tar -C "$HOME/toolchains" -xzf /tmp/i486-linux-musl-cross.tgz
export PATH="$HOME/toolchains/i486-linux-musl-cross/bin:$PATH"
i486-linux-musl-gcc --version
```

If your distro provides `grub-mkrescue` instead of `i686-elf-grub-mkrescue`, create a compatibility symlink or wrapper so the command name used by the Makefile exists.

### Linux

Package names vary by distro, but you need the same toolchain listed above.

Ubuntu / Debian:

```sh
sudo apt update
sudo apt install -y build-essential python3 curl nasm qemu-system-x86 xorriso grub-pc-bin mtools pandoc typst zip unzip perl librsvg2-bin clang-format clang-tidy cppcheck sparse
```

Fedora:

```sh
sudo dnf install -y make python3 curl nasm qemu-system-i386 xorriso grub2-tools-extra mtools pandoc typst zip unzip perl librsvg2-tools clang-tools-extra cppcheck sparse
```

Arch:

```sh
sudo pacman -S --needed make python curl nasm qemu-desktop xorriso grub mtools pandoc typst zip unzip perl librsvg clang cppcheck sparse
```

As on Windows, make sure the `x86_64-elf` cross compiler/linker and optional `i386-elf-gdb` are installed and visible on `PATH`.

Verify the compiler tools you need are on `PATH`:

```sh
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version
i486-linux-musl-gcc --version
```

If `i486-linux-musl-gcc` is not packaged by your distro, use the musl
cross-toolchain tarball shown in the Windows section and add its `bin`
directory to `PATH`.

If you use a different musl triplet, override the Makefile defaults:

```sh
make LINUX_I386_CC=<your-triplet>-gcc \
     LINUX_I386_CROSS_COMPILE=<your-triplet>- user/busybox
```

If your distro installs `grub-mkrescue` but not `i686-elf-grub-mkrescue`, add a symlink or wrapper with the `i686-elf-grub-mkrescue` name because that is what this repo's Makefile invokes.

## Build And Run

For a clean first x86 desktop boot, build the kernel, ISO, disk image, and launch QEMU with the desktop enabled:

```sh
make NO_DESKTOP=0 run-fresh
```

The shorter `make fresh` target is still available, but the default x86 build keeps the desktop disabled unless `NO_DESKTOP=0` is passed.

To rebuild the desktop image without launching QEMU, use:

```sh
make KTEST=0 NO_DESKTOP=0 kernel disk
```

When the desktop starts, it claims the framebuffer through the kernel display-ownership syscall. While that claim is active, kernel log text and legacy TTY keyboard echo are kept off the visible framebuffer, and keyboard events are delivered through `/dev/kbd` to the desktop instead of also feeding the old console path. Kernel logs remain available through the debug console and in-kernel log ring.

The mouse pointer sprite lives in `shared/cursor_sprite.h`, so the desktop compositor and kernel framebuffer cursor paths render the same 13x20 shape instead of maintaining separate ad hoc cursor drawings.

On x86, user ELF images are linked and loaded above the low kernel direct-map window at `0x08000000`. The loader rejects user mappings that overlap the protected low address range, the user stack, or another `PT_LOAD` segment, which keeps oversized or misplaced programs from silently corrupting kernel-owned memory.

For the AArch64 bring-up path, run the normal graphical console directly:

```sh
make ARCH=arm64 run
```

This opens the QEMU display and mirrors the ARM64 serial console to a
VGA-style framebuffer text console. Input remains serial-backed in this
milestone; ARM64 keyboard support is a separate follow-up phase.

On a normal x86 QEMU boot, Drunix opens the shell inside the framebuffer desktop. If the bootloader does not provide a usable 32-bit RGB framebuffer, the kernel falls back to the legacy VGA text presentation path.

The GRUB menu also offers two console-only entries. `Drunix (text console)`
passes `nodesktop` and uses the full framebuffer text grid when a framebuffer
is available. `Drunix (VGA text console)` boots `kernel-vga.elf`, whose
Multiboot header does not request a graphics framebuffer, and passes
`nodesktop vgatext` so the kernel uses the classic 80x25 VGA text backend.
Hold SHIFT at boot to interrupt the default, or edit `iso/boot/grub/grub.cfg`
to change `set default=0` to `set default=1` or `set default=2`.

Useful targets:

Common workflows:

- `make fresh` / `make run-fresh` rebuild `disk.img` as needed, then launch QEMU
- `make run` rebuilds the x86 kernel and ISO as needed, then launches QEMU without rebuilding `disk.img`
- `make build` builds the x86 bootable ISO and both disk images without launching QEMU
- `make check` runs the selected architecture's headless test suite plus
  static test-wiring and cross-architecture intent checks
- `make ARCH=arm64 run` boots the AArch64 target with the VGA-style framebuffer console enabled
- `make ARCH=arm64 check` runs the arm64 headless suite and the same shared
  test-wiring policy checks
- `make all` defaults to the fresh x86 boot workflow; under `ARCH=arm64` it aliases `run`
- `make rebuild` wipes build outputs, rebuilds the selected architecture's outputs, and boots from scratch
- `make clean` removes build outputs

Build-only targets:

- `make kernel` rebuilds `kernel.elf`, `kernel-vga.elf`, and `os.iso`
- `make iso` rebuilds `os.iso`
- `make disk` / `make images` rebuild `img/disk.img` and `img/dufs.img`
- `make ARCH=arm64 build` rebuilds `kernel-arm64.elf`, `kernel8.img`, and
  the arm64 root disk image

Static analysis and style targets:

- `make compile-commands` regenerates `compile_commands.json` for local tools
  such as `cppcheck`, `clangd`, and `clang-tidy`
- `make format-check` runs `clang-format --dry-run --Werror` using the repo's
  `.clang-format` policy
- `make cppcheck` runs Cppcheck against the generated compilation database
- `make clang-tidy-include-check` runs clang-tidy's `misc-include-cleaner`
  check over kernel C sources to catch unused or missing direct includes
- `make sparse-check` runs Sparse over kernel C sources with the kernel build
  flags
- `make scan` runs all scanner targets

The scanner targets fail by default when they find issues. Use `SCAN_FAIL=0`
for reporting-only cleanup passes, for example `make SCAN_FAIL=0 cppcheck`.

Run and debug targets:

- `make run-stdio` same as `run` but streams QEMU's debug console output to the terminal
- `make run-grub-menu` boots into the GRUB menu so you can pick `nodesktop` or VGA-text entries by hand
- `make debug` starts QEMU paused with the GDB remote stub and kernel symbols loaded
- `make debug-fresh` rebuilds `img/disk.img` first, then starts `make debug`
- `make debug-user APP=shell` starts `debug` and loads symbols for `user/shell`

For `ARCH=arm64`, `run-stdio` and `run-grub-menu` are simple aliases of `run`.
`make ARCH=arm64 debug` starts QEMU paused with the AArch64 GDB remote stub,
and `make ARCH=arm64 debug-user APP=shell` adds symbols for the selected arm64
user binary at the fixed user load address.

In-kernel tests (KTEST):

- `make test` boots with the in-kernel unit tests enabled. Test output is
  routed silently to `logs/debugcon.log` and `/proc/kmsg`, so the on-screen
  desktop is visually identical to `make run` and you can inspect visual
  bugs while the suite also runs. Grep `logs/debugcon.log` for
  `KTEST: SUMMARY pass=N fail=M` to see the result.
- `make test-fresh` same as `test` but rebuilds `img/disk.img` first
- `make test-headless` runs the selected architecture's headless KTEST path and
  related shared smoke checks. On x86 it builds with tests enabled, boots QEMU
  with `-display none`, waits for the summary line in
  `logs/debugcon-ktest.log`, and exits non-zero if any case failed.
- `make check` runs `test-headless` plus static checks that public test targets
  and intents stay architecture-neutral across x86 and arm64.
- `make KTEST=1 run` equivalent to `make test`

Halt-inducing and userland integration tests (all headless):

- `make test-halt` verifies the double-fault path via a dedicated TSS
- `make test-linux-abi` boots a static Linux/i386 ELF and checks syscall
  return values and errno-compatible negatives
- `make test-threadtest` boots a raw `clone(2)` threading smoke test and
  checks shared memory, TID writes, and clear-child-TID cleanup
- `make test-busybox-compat` runs the unattended BusyBox compatibility suite
  and extracts the on-disk report
- `make test-ext3-linux-compat` generates an ext3 root, validates it with
  host `e2fsprogs`, boots the in-kernel ext3 writer smoke tests, and fscks
  the mutated image
- `make test-ext3-host-write-interop` uses host `debugfs` to write into the
  ext3 image, reads it back, and fscks the result
- `make validate-ext3-linux` runs the host ext3 validators (`e2fsck`,
  `dumpe2fs`, and the repo's compat checkers) without booting QEMU
- `make test-all` runs the in-kernel unit tests, the Linux ABI smoke, and
  the halt-inducing tests in sequence; exits non-zero if any fail

Documentation:

- `make epub` builds the EPUB edition
- `make pdf` builds the PDF book from the Markdown sources with Pandoc and Typst
- `make docs` builds both the EPUB and the PDF

### Build System Layout

The root `Makefile` is the entry point for architecture selection, global build
options, sentinels, common pattern rules, and the small amount of kernel-link
wiring that has to sit near the object lists. Specialized target families live
in included fragments so the root file stays small:

- `kernel/objects.mk` lists the normal and VGA kernel object sets
- `kernel/tests.mk` lists in-kernel test objects, included only when `KTEST=1`
- `user/programs.mk` is the shared user program manifest used by both the root
  `Makefile` and `user/Makefile`
- `mk/disk-images.mk` builds `disk.fs`, `dufs.fs`, `img/disk.img`, and
  `img/dufs.img` for both x86 and arm64 root filesystem layouts
- `mk/checks.mk` owns the shared `check-*` wiring and architecture-neutral test
  policy checks
- `mk/scan-x86.mk` and `mk/scan-arm64.mk` generate `compile_commands.json` and
  run the scanner targets for each architecture
- `mk/run-x86.mk` and `mk/run-arm64.mk` own the run, debug, and architecture
  specific test entry points
- `mk/utility-targets.mk` owns common aliases such as `all`, `rebuild`,
  `clean`, and the public `.PHONY` target list
- `docs/sources.mk` lists book chapter sources
- `docs/build.mk` contains the EPUB, PDF, and diagram build rules
- `test/targets.mk` contains the headless and integration test targets

`make check` runs `tools/test_makefile_decomposition.py` so new target families
do not quietly accumulate back in the root `Makefile`.

Contributor policy lives under `docs/contributing/`. Use
`docs/contributing/c-style.md` for C formatting and cleanup rules,
`docs/style.md` plus `docs/contributing/docs.md` for book prose and chapter
workflow, and the other files in `docs/contributing/` for focused project
rules such as syscall-table maintenance, commit messages, Linux references,
cross-architecture testing policy (`docs/contributing/testing.md`), and README
updates.

When adding a user program to the disk image, update `user/programs.mk` and add
the build rule or source file in `user/Makefile` as needed. Native Drunix C
programs go in `C_PROGS`, native Drunix C++ programs go in `CXX_PROGS`, and
static Linux/i386 compatibility programs go in `LINUX_PROGS`. The root
Makefile derives disk image contents from `PROGS`, so it does not need another
per-program edit.

### Root filesystem selection

`disk.img` is an MBR disk image. Its first primary partition appears as
`/dev/sda1` inside Drunix and holds the configured root filesystem. The default
root is ext3. `dufs.img` is the secondary MBR disk image; its first primary
partition appears as `/dev/sdb1` and is mounted at `/dufs` during ext3-root
boots.

Build with `ROOT_FS=dufs` to use the legacy DUFS filesystem as the root instead:

```sh
make ROOT_FS=dufs run-fresh
```

### Build Options

Builds default to production optimization:

```sh
make build
```

The default `BUILD_MODE=production` selects `-O2` for kernel and native user
objects on both architectures. Use `BUILD_MODE=debug` when you want debug
optimization without switching target names:

```sh
make BUILD_MODE=debug build
make ARCH=arm64 BUILD_MODE=debug kernel
```

The interactive debug targets also force debug optimization automatically:
`make debug`, `make debug-user APP=shell`, and `make debug-fresh` compile with
`-Og` before launching QEMU/GDB.

Mouse cursor speed in the framebuffer desktop is controlled at build time:

```sh
make MOUSE_SPEED=6 os.iso
```

`MOUSE_SPEED` defaults to `4`. Values below `1` use `1`; values above `16` use
`16`. The option affects framebuffer mouse motion only. VGA text fallback
pointer motion remains unscaled, at one pixel per raw mouse unit before cell
coordinates are derived.

To compile out the desktop entirely, useful for text-console-only builds or
to shave a few KB of `.text`, build with `NO_DESKTOP=1`:

```sh
make NO_DESKTOP=1 run-fresh
```

The kernel skips `desktop_init` and boots straight to the console. With a
framebuffer, this uses the full framebuffer text grid. The runtime `nodesktop`
GRUB cmdline flag is still honored on a normal build, so you only need
`NO_DESKTOP=1` when you want the desktop gone at compile time.

To force the 80x25 VGA text backend at build time, use `VGA_TEXT=1`:

```sh
make VGA_TEXT=1 run-fresh
```

`VGA_TEXT=1` implies `DRUNIX_NO_DESKTOP` and defines `DRUNIX_VGA_TEXT`, so the
kernel ignores a Multiboot framebuffer even if one is reported. Lowercase
aliases `no_desktop=1` and `vga_text=1` are also accepted.

### Userland C++ Support

User programs can be written in C or in a freestanding C++ subset. C programs
continue to compile with `x86_64-elf-gcc`; C++ programs compile with
`x86_64-elf-g++` and link against the repo-owned user runtime in `user/runtime`.

The current C++ userland supports global constructors and destructors,
classes, virtual dispatch, `new`, `delete`, `new[]`, and `delete[]`.
Allocation uses the existing `malloc` and `free` implementation backed by
`SYS_BRK`.

Exceptions, RTTI, `libstdc++`, and `libsupc++` are not part of the current
runtime. Code that depends on those features should fail at compile or link
time instead of pulling in hosted runtime libraries implicitly.

The C smoke binary is `/bin/chello`, built from `user/apps/chello.c`. The C++
smoke binary is `/bin/cpphello`, built from `user/apps/cpphello.cpp`. The Linux
i386 ABI smoke binary is `/bin/linuxhello`, built from handwritten assembly
that invokes Linux `write(2)` and `exit(2)` syscall numbers directly.
`user/programs.mk` keeps the runtime lanes explicit: C programs link the C
runtime objects, C++ programs link those same C runtime objects plus the C++
runtime objects, and Linux compatibility binaries link no Drunix runtime at all.
The book-level walkthrough is Chapter 30, `docs/ch30-cpp-userland.md`.

## Debugging

For the mainline x86 target, start QEMU paused with a GDB stub and attach GDB:

```sh
make debug
```

Other useful debug flows:

- `make run-stdio` streams QEMU debug console output to your terminal
- `make KLOG_TO_DEBUGCON=1 run` mirrors ordinary `klog()` output to QEMU debugcon
- `make test-halt` boots a special image that verifies the dedicated double-fault path

For the AArch64 target, use `make ARCH=arm64 run` for the graphical
framebuffer mirror, or `make ARCH=arm64 check` for the headless test suite and
test-wiring policy checks.

Runtime logs:

- `logs/serial.log` captures COM1 output
- `logs/debugcon.log` captures QEMU debug console output on port `0xE9`
- `logs/serial-arm.log` captures the headless AArch64 bring-up serial log used by `make ARCH=arm64 check`

Fatal kernel faults write diagnostics to serial and debugcon so they remain visible even when the framebuffer or VGA display path is no longer reliable.

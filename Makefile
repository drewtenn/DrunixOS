CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
NASM    := nasm
PYTHON  := python3
QEMU    := qemu-system-i386
GDB     := i386-elf-gdb
E2FSPROGS_SBIN ?= /opt/homebrew/opt/e2fsprogs/sbin
E2FSCK  ?= $(if $(wildcard $(E2FSPROGS_SBIN)/e2fsck),$(E2FSPROGS_SBIN)/e2fsck,e2fsck)
DUMPE2FS ?= $(if $(wildcard $(E2FSPROGS_SBIN)/dumpe2fs),$(E2FSPROGS_SBIN)/dumpe2fs,dumpe2fs)
DEBUGFS ?= $(if $(wildcard $(E2FSPROGS_SBIN)/debugfs),$(E2FSPROGS_SBIN)/debugfs,debugfs)
CFLAGS  := -m32 -g -ffreestanding -mno-sse -mno-sse2 -mno-mmx -msoft-float -Wstack-usage=1024
INC     := -I kernel -I kernel/arch -I kernel/mm -I kernel/drivers -I kernel/blk -I kernel/proc -I kernel/fs -I kernel/lib -I kernel/gui
DEPFLAGS := -MMD -MP
MOUSE_SPEED ?= 4
INIT_PROGRAM ?= bin/shell
INIT_ARG0 ?= shell
INIT_ENV0 ?= PATH=/bin
ROOT_FS ?= ext3
CFLAGS += -DMOUSE_FRAMEBUFFER_PIXEL_SCALE=$(MOUSE_SPEED)
CFLAGS += -DDRUNIX_INIT_PROGRAM=\"$(INIT_PROGRAM)\"
CFLAGS += -DDRUNIX_INIT_ARG0=\"$(INIT_ARG0)\"
CFLAGS += -DDRUNIX_INIT_ENV0=\"$(INIT_ENV0)\"
CFLAGS += -DDRUNIX_ROOT_FS=\"$(ROOT_FS)\"
NASMFLAGS :=

# Build with NO_DESKTOP=1 to skip desktop init entirely and boot straight to
# the legacy console. The runtime "nodesktop" cmdline flag (set via grub) is
# still honored even when the desktop is compiled in.
ifneq ($(origin no_desktop),undefined)
NO_DESKTOP ?= $(no_desktop)
endif
NO_DESKTOP ?= 0
ifeq ($(NO_DESKTOP),1)
CFLAGS += -DDRUNIX_NO_DESKTOP
endif
ifneq ($(origin vga_text),undefined)
VGA_TEXT ?= $(vga_text)
endif
VGA_TEXT ?= 0
ifeq ($(VGA_TEXT),1)
CFLAGS += -DDRUNIX_NO_DESKTOP -DDRUNIX_VGA_TEXT
NASMFLAGS += -DDRUNIX_VGA_TEXT
endif

# ─── Unit tests ──────────────────────────────────────────────────────────────
# Build with KTEST=1 to compile the in-kernel test suite and run it at boot:
#   make test          (builds with tests enabled and launches QEMU)
KTEST ?= 0
ifeq ($(KTEST),1)
KLOG_TO_DEBUGCON ?= 1
CFLAGS += -DKTEST_ENABLED
INC    += -I kernel/test
KTOBJS  = kernel/test/ktest.o \
           kernel/test/test_pmm.o \
           kernel/test/test_kheap.o \
           kernel/test/test_vfs.o \
           kernel/test/test_process.o \
           kernel/test/test_sched.o \
           kernel/test/test_fs.o \
           kernel/test/test_uaccess.o \
           kernel/test/test_desktop.o \
           kernel/test/test_blkdev.o
else
KLOG_TO_DEBUGCON ?= 0
KTOBJS  =
endif

DOUBLE_FAULT_TEST ?= 0
ifeq ($(DOUBLE_FAULT_TEST),1)
CFLAGS += -DDOUBLE_FAULT_TEST
endif

ifeq ($(KLOG_TO_DEBUGCON),1)
CFLAGS += -DKLOG_TO_DEBUGCON
endif

# Sentinel: recompile kernel.c whenever KTEST flips between 0 and 1.
.ktest-flag: FORCE
	echo "$(KTEST)" | cmp -s - $@ || echo "$(KTEST)" > $@
.double-fault-test-flag: FORCE
	echo "$(DOUBLE_FAULT_TEST)" | cmp -s - $@ || echo "$(DOUBLE_FAULT_TEST)" > $@
.klog-debugcon-flag: FORCE
	echo "$(KLOG_TO_DEBUGCON)" | cmp -s - $@ || echo "$(KLOG_TO_DEBUGCON)" > $@
.mouse-speed-flag: FORCE
	echo "$(MOUSE_SPEED)" | cmp -s - $@ || echo "$(MOUSE_SPEED)" > $@
.init-program-flag: FORCE
	printf '%s\n%s\n%s\n%s\n' "$(INIT_PROGRAM)" "$(INIT_ARG0)" "$(INIT_ENV0)" "$(ROOT_FS)" | cmp -s - $@ || printf '%s\n%s\n%s\n%s\n' "$(INIT_PROGRAM)" "$(INIT_ARG0)" "$(INIT_ENV0)" "$(ROOT_FS)" > $@
.no-desktop-flag: FORCE
	echo "$(NO_DESKTOP)" | cmp -s - $@ || echo "$(NO_DESKTOP)" > $@
.vga-text-flag: FORCE
	echo "$(VGA_TEXT)" | cmp -s - $@ || echo "$(VGA_TEXT)" > $@
# GRUB menu timeout (seconds). Default 0 boots the first entry instantly.
# Override (e.g. GRUB_TIMEOUT=10) to display the menu — the `run-grub-menu`
# target uses this for interactive verification.
GRUB_TIMEOUT ?= 0
FORCE:

kernel/kernel.o: .ktest-flag FORCE
kernel/kernel.o: .double-fault-test-flag
kernel/kernel.o: .init-program-flag
kernel/kernel.o: .no-desktop-flag
kernel/kernel.o: .vga-text-flag
kernel/kernel-entry.o: .vga-text-flag FORCE
kernel/lib/klog.o: .klog-debugcon-flag
kernel/drivers/mouse.o: .mouse-speed-flag
kernel/test/test_desktop.o: .mouse-speed-flag

# GRUB2 mkrescue (provided by: brew install i686-elf-grub xorriso)
GRUB_MKRESCUE := i686-elf-grub-mkrescue
ISO_KERNEL    := iso/boot/kernel.elf
ISO_KERNEL_VGA := iso/boot/kernel-vga.elf
DISK_SECTORS  := 102400
PARTITION_START ?= 2048
FS_SECTORS     := $(shell expr $(DISK_SECTORS) - $(PARTITION_START))
USER_PROGS    := shell chello hello writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello lsblk linuxhello linuxprobe linuxabi busybox tcc bbcompat dufstest redirtest ext3wtest threadtest tcccompat
USER_BINS     := $(addprefix user/,$(USER_PROGS))
USER_RUNTIME_HEADERS := $(wildcard user/lib/*.h)
USER_RUNTIME_SYSROOT := $(foreach hdr,$(USER_RUNTIME_HEADERS),$(hdr) usr/include/$(notdir $(hdr))) \
                        user/lib/tcc_crt0.o usr/lib/drunix/crt0.o \
                        user/lib/libc.a usr/lib/drunix/libc.a \
                        user/user.ld usr/lib/drunix/user.ld
DISK_FILES    := $(foreach prog,$(USER_PROGS),user/$(prog) bin/$(prog)) \
                 $(USER_RUNTIME_SYSROOT) \
                 tools/hello.txt hello.txt \
                 tools/readme.txt readme.txt
RUN_LOGS      := serial.log debugcon.log
TEST_SUFFIXES := ktest df bbcompat linuxabi ext3w threadtest tcc
TEST_IMAGES   := $(foreach suffix,$(TEST_SUFFIXES),disk-$(suffix).img dufs-$(suffix).img) disk-ext3-host.img
TEST_LOGS     := $(foreach suffix,$(TEST_SUFFIXES),serial-$(suffix).log debugcon-$(suffix).log) \
                 bbcompat.log linuxabi.log ext3wtest.log threadtest.log tcc.log ext3-host-readback.txt
SENTINELS     := .ktest-flag .double-fault-test-flag .klog-debugcon-flag \
                 .mouse-speed-flag .init-program-flag .no-desktop-flag .vga-text-flag

QEMU_DISKS    = -drive format=raw,file=$(1),if=ide,index=0 \
                 -drive format=raw,file=$(2),if=ide,index=1
QEMU_BOOT     := -cdrom os.iso -boot d -no-reboot -no-shutdown \
                 -global isa-debugcon.iobase=0xe9
QEMU_COMMON   := $(call QEMU_DISKS,disk.img,dufs.img) $(QEMU_BOOT)
QEMU_LOGS     := -serial file:serial.log -debugcon file:debugcon.log
GDB_COMMON    := -ex "set pagination off" \
                 -ex "set confirm off" \
                 -ex "set tcp auto-retry on" \
                 -ex "file kernel.elf"

define qemu_run
rm -f $(RUN_LOGS)
$(QEMU) $(QEMU_COMMON) $(QEMU_LOGS)
endef

define qemu_debug
rm -f $(RUN_LOGS)
$(QEMU) -s -S \
    $(QEMU_COMMON) \
    $(QEMU_LOGS) &
sleep 1
$(GDB) $(GDB_COMMON) $(1) \
       -ex "target remote localhost:1234" \
       -ex "hbreak idt_init_early" \
       -ex "continue"
endef

define prepare_test_images
rm -f serial-$(1).log debugcon-$(1).log disk-$(1).img dufs-$(1).img $(2)
cp -f disk.img disk-$(1).img
cp -f dufs.img dufs-$(1).img
endef

define qemu_headless_for
sh -c '$(QEMU) -display none $(call QEMU_DISKS,disk-$(1).img,dufs-$(1).img) $(QEMU_BOOT) -serial file:serial-$(1).log -debugcon file:debugcon-$(1).log >/dev/null 2>&1 & pid=$$!; sleep $(2); kill $$pid >/dev/null 2>&1 || true; wait $$pid >/dev/null 2>&1 || true'
endef

define qemu_headless_until_log
sh -c '$(QEMU) -display none $(call QEMU_DISKS,disk-$(1).img,dufs-$(1).img) $(QEMU_BOOT) -serial file:serial-$(1).log -debugcon file:debugcon-$(1).log >/dev/null 2>&1 & pid=$$!; for i in $$(seq 1 $(2)); do grep -q "$(3)" debugcon-$(1).log 2>/dev/null && break; sleep 1; done; kill $$pid >/dev/null 2>&1 || true; wait $$pid >/dev/null 2>&1 || true'
endef

# ─── Pattern rules ───────────────────────────────────────────────────────────
%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

%.o: %.asm
	$(NASM) $(NASMFLAGS) $< -f elf -o $@

# ─── Kernel link ─────────────────────────────────────────────────────────────
KOBJS = kernel/kernel-entry.o kernel/kernel.o \
        kernel/module.o kernel/module_exports.o \
        kernel/lib/klog.o \
        kernel/lib/kstring.o kernel/lib/kprintf.o kernel/lib/ksort.o \
        kernel/arch/gdt.o kernel/arch/gdt_flush.o \
        kernel/arch/idt.o kernel/arch/isr.o kernel/arch/sse.o kernel/arch/df_test.o \
        kernel/arch/irq.o kernel/arch/pit.o kernel/arch/clock.o \
        kernel/drivers/keyboard.o kernel/drivers/mouse.o kernel/drivers/ata.o \
        kernel/drivers/blkdev.o kernel/drivers/blkdev_part.o kernel/blk/bcache.o kernel/drivers/chardev.o kernel/drivers/tty.o \
        kernel/gui/display.o kernel/gui/framebuffer.o kernel/gui/font8x16.o kernel/gui/desktop.o kernel/gui/desktop_apps.o kernel/gui/terminal.o \
        kernel/mm/pmm.o kernel/mm/paging.o kernel/mm/paging_asm.o kernel/mm/fault.o kernel/mm/vma.o kernel/mm/kheap.o kernel/mm/slab.o \
        kernel/proc/elf.o kernel/proc/process.o kernel/proc/process_asm.o kernel/proc/task_group.o kernel/proc/resources.o \
        kernel/proc/sched.o kernel/proc/syscall.o kernel/proc/core.o kernel/proc/mem_forensics.o kernel/proc/pipe.o kernel/proc/switch.o \
        kernel/proc/uaccess.o \
        kernel/fs/fs.o kernel/fs/vfs.o kernel/fs/procfs.o kernel/fs/sysfs.o kernel/fs/ext3.o

$(KOBJS): .ktest-flag

kernel.elf: $(KOBJS) $(KTOBJS)
	$(LD) -m elf_i386 -o $@ -T kernel/kernel.ld $(KOBJS) $(KTOBJS)

kernel/kernel-entry-vga.o: kernel/kernel-entry.asm
	$(NASM) -DDRUNIX_VGA_TEXT $< -f elf -o $@

KOBJS_VGA = kernel/kernel-entry-vga.o $(filter-out kernel/kernel-entry.o,$(KOBJS))

kernel-vga.elf: kernel/kernel-entry-vga.o $(filter-out kernel/kernel-entry.o,$(KOBJS)) $(KTOBJS)
	$(LD) -m elf_i386 -o $@ -T kernel/kernel.ld $(KOBJS_VGA) $(KTOBJS)

# ─── User programs ───────────────────────────────────────────────────────────
# Declared phony so make always delegates to the user subdirectory's own
# dependency tracking — changes to user/*.c or user/lib/* are picked up
# without needing a manual clean.
.PHONY: $(USER_BINS)
$(USER_BINS):
	$(MAKE) -C user $(@F)

user/lib/libc.a user/lib/tcc_crt0.o:
	$(MAKE) -C user $(@F:%=lib/%)

# ─── Hard-disk images ────────────────────────────────────────────────────────
# disk.img is the primary ATA master (sda).  By default it is a deterministic
# Linux-compatible ext3 root partition.  ROOT_FS=dufs builds sda as DUFS
# instead.
ifeq ($(ROOT_FS),dufs)
disk.fs: $(USER_BINS) user/lib/libc.a user/lib/tcc_crt0.o tools/hello.txt tools/readme.txt tools/mkfs.py
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS) $(DISK_FILES)
disk.img: disk.fs tools/wrap_mbr.py
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA
else
disk.fs: $(USER_BINS) user/lib/libc.a user/lib/tcc_crt0.o tools/hello.txt tools/readme.txt tools/mkext3.py
	$(PYTHON) tools/mkext3.py $@ $(FS_SECTORS) $(DISK_FILES)
disk.img: disk.fs tools/wrap_mbr.py
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0x83
endif

# dufs.img is the primary ATA slave (sdb), mounted at /dufs during ext3-root
# boots.  It is intentionally not rebuilt by run-fresh when it already exists.
dufs.fs: tools/mkfs.py
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS)
dufs.img: dufs.fs tools/wrap_mbr.py
	$(PYTHON) tools/wrap_mbr.py dufs.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA

# ─── Documentation ───────────────────────────────────────────────────────────
DOCS_SRC := docs/partI-firmware-to-kernel.md \
            docs/ch01-boot.md \
            docs/ch02-protected-mode.md \
            docs/ch03-kernel-entry.md \
            docs/ch04-interrupts.md \
            docs/ch05-irq-dispatch.md \
            docs/ch06-sse.md \
            docs/partII-memory.md \
            docs/ch07-memory-detection.md \
            docs/ch08-memory-management.md \
            docs/partIII-hardware-interfaces.md \
            docs/ch09-klog.md \
            docs/ch10-keyboard.md \
            docs/ch11-ata-disk.md \
            docs/ch12-device-registries.md \
            docs/partIV-files-and-storage.md \
            docs/ch13-filesystem.md \
            docs/ch14-vfs.md \
            docs/partV-running-user-programs.md \
            docs/ch15-processes.md \
            docs/ch16-syscalls.md \
            docs/ch17-file-io.md \
            docs/partVI-user-environment.md \
            docs/ch18-tty.md \
            docs/ch19-signals.md \
            docs/ch20-user-runtime.md \
            docs/ch21-libc.md \
            docs/ch22-shell.md \
            docs/partVII-extending-the-kernel.md \
            docs/ch23-modules.md \
            docs/ch24-core-dumps.md \
            docs/partVIII-fault-driven-memory.md \
            docs/ch25-demand-paging.md \
            docs/ch26-copy-on-write-fork.md \
            docs/partIX-graphical-environment.md \
            docs/ch27-mouse.md \
            docs/ch28-desktop.md \
            docs/partX-development-tools.md \
            docs/ch29-debugging.md \
            docs/ch30-cpp-userland.md

EPUB_FRONTMATTER := docs/epub-copyright.md
PDF_FRONTMATTER := docs/epub-copyright.md
PDF_METADATA := docs/pdf-metadata.yaml
PDF_FILTER := docs/pdf.lua
PDF_TEMPLATE := docs/pdf-template.typ

PDF  := docs/Drunix OS.pdf
EPUB := docs/Drunix OS.epub

# ─── Diagram pipeline ────────────────────────────────────────────────────────
# docs/generate_diagrams.py is the source of truth for generated diagrams.
# It writes SVG files into docs/diagrams/. docs/extract-diagrams.py remains
# as the one-time ASCII-art migration helper.
#
# epub build: SVGs are converted to PNG (2× scale) so Apple Books can
#             tap-to-zoom. The Lua filter rewrites .svg paths to .png.
# PDF build:  Pandoc renders the markdown sources through Typst for stable
#             book typography while keeping SVG diagrams vector-sharp.

DIAGRAMS_SVG := $(wildcard docs/diagrams/*.svg)
DIAGRAMS_PNG := $(DIAGRAMS_SVG:.svg=.png)

# Convert SVG diagrams to 2× PNG for the epub (Apple Books requires raster
# images for tap-to-zoom). Prefer librsvg's rsvg-convert; fall back to
# CairoSVG only when rsvg-convert is not installed.
docs/diagrams/%.png: docs/diagrams/%.svg
	@if command -v rsvg-convert >/dev/null 2>&1; then \
		rsvg-convert -f png -z 2 -o "$@" "$<"; \
	elif $(PYTHON) -c "import cairosvg" >/dev/null 2>&1; then \
		$(PYTHON) -c "import cairosvg; cairosvg.svg2png(url='$<', write_to='$@', scale=2)"; \
	else \
		echo "error: need the 'rsvg-convert' binary from librsvg or Python package 'cairosvg' to build EPUB diagrams" >&2; \
		exit 1; \
	fi

docs/Drunix\ OS.pdf: $(PDF_FRONTMATTER) $(DOCS_SRC) $(DIAGRAMS_SVG) docs/cover-art.png docs/epub-metadata.yaml $(PDF_METADATA) $(PDF_FILTER) $(PDF_TEMPLATE)
	pandoc $(PDF_FRONTMATTER) $(DOCS_SRC) \
	    --to typst \
	    --standalone \
	    --template "$(PDF_TEMPLATE)" \
	    --metadata-file docs/epub-metadata.yaml \
	    --metadata-file "$(PDF_METADATA)" \
	    --lua-filter "$(PDF_FILTER)" \
	    --syntax-highlighting=monochrome \
	    --resource-path=docs \
	    --pdf-engine=typst \
	    -o "$(PDF)"

# ─── epub ─────────────────────────────────────────────────────────────────────
# Use the generated PNG as the actual EPUB cover image.
# --epub-title-page=false suppresses pandoc's plain auto-generated title page.
# --split-level=2 puts each chapter (H2) in its own XHTML file. The Lua filter
# strips \newpage and other LaTeX-only constructs and rewrites diagram paths
# from .svg to .png so Apple Books enables tap-to-zoom on every diagram.
docs/Drunix\ OS.epub: $(EPUB_FRONTMATTER) $(DOCS_SRC) $(DIAGRAMS_PNG) docs/cover-art.png docs/epub.css docs/epub-metadata.yaml docs/strip-latex.lua
	tmpdir="$$(mktemp -d /tmp/drunix-epub.XXXXXX)"; \
	pandoc $(EPUB_FRONTMATTER) $(DOCS_SRC) \
	    --to epub3 \
	    --toc \
	    --toc-depth=2 \
	    --split-level=2 \
	    --epub-title-page=false \
	    --epub-cover-image=docs/cover-art.png \
	    --css docs/epub.css \
	    --metadata-file docs/epub-metadata.yaml \
	    --lua-filter docs/strip-latex.lua \
	    --syntax-highlighting=monochrome \
	    --resource-path=docs \
	    -o "$$tmpdir/book.epub"; \
	cd "$$tmpdir" && unzip -q book.epub -d unpacked; \
	perl -0pi -e 's|<itemref idref="cover_xhtml" />\s*<itemref idref="nav" />\s*<itemref idref="ch001_xhtml" />|<itemref idref="cover_xhtml" />\n    <itemref idref="ch001_xhtml" />\n    <itemref idref="nav" />|s' "$$tmpdir/unpacked/EPUB/content.opf"; \
	rm -f "$(EPUB)"; \
	cd "$$tmpdir/unpacked" && zip -X0 "../book-fixed.epub" mimetype >/dev/null && zip -Xr9D "../book-fixed.epub" META-INF EPUB >/dev/null; \
	mv "$$tmpdir/book-fixed.epub" "$(CURDIR)/$(EPUB)"; \
	rm -rf "$$tmpdir"

# ─────────────────────────────────────────────────────────────────────────────
# BUILD TARGETS  (compile/link only — do not launch QEMU)
# ─────────────────────────────────────────────────────────────────────────────

# `kernel`  — compile and link the kernel, then build the bootable GRUB ISO.
#             Does NOT touch disk.img.
$(ISO_KERNEL): kernel.elf
	cp $< $@

$(ISO_KERNEL_VGA): kernel-vga.elf
	cp $< $@

# Regenerate grub.cfg only when the rendered output differs from what's on
# disk — keeps mtime stable so os.iso isn't relinked on every invocation.
iso/boot/grub/grub.cfg: iso/boot/grub/grub.cfg.in FORCE
	@sed 's/@TIMEOUT@/$(GRUB_TIMEOUT)/' $< | cmp -s - $@ 2>/dev/null || \
		sed 's/@TIMEOUT@/$(GRUB_TIMEOUT)/' $< > $@

os.iso: $(ISO_KERNEL) $(ISO_KERNEL_VGA) iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ iso

kernel: os.iso

# `disk`    — build the hard-disk images with all compiled user programs.
#             Use this to (re)populate the filesystem after adding/changing user
#             programs.  Most run/debug targets intentionally do NOT depend on
#             this so that the filesystem state is preserved across boots.
disk: disk.img dufs.img

# Short workflow aliases.
build: kernel disk
iso: os.iso
images: disk
fresh: run-fresh
check: test-headless

validate-ext3-linux: disk.img tools/check_ext3_linux_compat.py tools/check_ext3_journal_activity.py
	$(PYTHON) tools/check_ext3_linux_compat.py disk.img
	$(E2FSCK) -fn disk.fs
	$(DUMPE2FS) -h disk.fs | grep -q 'Filesystem features:.*has_journal'
	$(DUMPE2FS) -h disk.fs | grep -q 'Journal inode:[[:space:]]*8'

# `pdf`    — render the PDF book from the markdown chapter sources.
pdf: docs/Drunix\ OS.pdf

# `epub`   — render the EPUB edition of the book.
epub: docs/Drunix\ OS.epub

# `docs`   — build all documentation formats: PDF and EPUB.
docs: pdf epub

# ─────────────────────────────────────────────────────────────────────────────
# RUN TARGETS  (boot QEMU with the current disk.img — no filesystem rebuild)
# ─────────────────────────────────────────────────────────────────────────────

# `run`     — boot the OS in QEMU.  Uses whatever disk.img is already on disk;
#             does NOT rebuild the filesystem.  Logs written to serial.log and
#             debugcon.log.
run: os.iso dufs.img
	$(call qemu_run)

# `run-grub-menu` — boot with the GRUB menu visible for 10 seconds so the
#                   menu entries can be exercised interactively.  Default
#                   builds keep timeout=0 (boots straight into the default).
run-grub-menu:
	$(MAKE) GRUB_TIMEOUT=10 run

# `run-stdio` — same as `run` but routes the debugcon port to the terminal
#               instead of a file.  Useful when you want to tail kernel log
#               output live without opening debugcon.log separately.
run-stdio: os.iso dufs.img
	$(QEMU) $(QEMU_COMMON) -serial file:serial.log -debugcon stdio

# `debug`   — start QEMU paused with the GDB remote stub active, then attach
#             GDB loaded with kernel symbols. Uses a hardware breakpoint at
#             `idt_init_early` so the first stop lands at a point that is safe
#             for source-level stepping. Set breakpoints, then `continue` to
#             boot. Does NOT rebuild the filesystem.
#             Port: localhost:1234 (QEMU default).
debug: os.iso dufs.img
	$(call qemu_debug,)

# `debug-user` — like `debug` but also loads symbols for a user-space program
#                so you can set breakpoints and step through user code.
#                The binary is expected at user/$(APP); symbols are added at the
#                ELF preferred load address (offset 0).
#
#                Usage:  make debug-user APP=shell
#
#                If the program is loaded at a non-default address by the kernel
#                ELF loader, adjust with GDB's `add-symbol-file user/<app> <addr>`
#                after connecting.
debug-user: os.iso dufs.img
	@test -n "$(APP)" || (echo "Usage: make debug-user APP=<program name>  (e.g. APP=shell)"; exit 1)
	$(call qemu_debug,-ex "add-symbol-file user/$(APP) 0x400000")

# ─────────────────────────────────────────────────────────────────────────────
# RUN + FRESH FILESYSTEM  (rebuild disk.img before booting)
# ─────────────────────────────────────────────────────────────────────────────

# `run-fresh`   — rebuild the root disk image from scratch, then boot the OS.
#                 Use this after adding or changing user programs to get a clean
#                 filesystem state.
run-fresh: disk.img
	$(MAKE) run

# `debug-fresh` — rebuild the root disk image from scratch, then boot paused
#                 under the GDB stub.  Combines `run-fresh` and `debug`.
debug-fresh: disk.img
	$(MAKE) debug

# ─────────────────────────────────────────────────────────────────────────────
# TEST TARGETS
# ─────────────────────────────────────────────────────────────────────────────

# `test`    — compile with KTEST=1 (in-kernel test suite) and boot under QEMU
#             with the normal display. Tests run silently during boot — their
#             output goes to debugcon.log / /proc/kmsg only, not the on-screen
#             console — so the desktop is visually identical to `make run` and
#             you can inspect visual bugs while tests also executed. Grep
#             `debugcon.log` for `KTEST: SUMMARY pass=N fail=M`.
#             Does NOT rebuild the filesystem.
test:
	$(MAKE) KTEST=1 run

# `test-fresh` — same as `test` but rebuilds the filesystem first.
#                Use when test cases rely on specific files being present on disk.
test-fresh:
	$(MAKE) KTEST=1 run-fresh

# `test-headless` — build with KTEST=1, boot headlessly, wait for the summary
#                   line in debugcon-ktest.log, then exit non-zero if any test
#                   case failed. Use in CI / scripted runs where no human will
#                   look at the framebuffer. Does NOT rebuild the filesystem.
test-headless:
	$(MAKE) KTEST=1 kernel disk
	$(call prepare_test_images,ktest,)
	$(call qemu_headless_until_log,ktest,60,KTEST.*SUMMARY pass=)
	grep -q "KTEST.*SUMMARY pass=" debugcon-ktest.log
	grep -q "KTEST.*SUMMARY pass=[0-9][0-9]* fail=0" debugcon-ktest.log

# `test-halt` — run halt-inducing kernel tests headlessly.  QEMU is launched
#               without a display and killed after a short timeout; the exit
#               status of the target reflects whether the expected panic fired.
#               Currently verifies the double-fault path via a dedicated TSS.
test-halt:
	$(MAKE) DOUBLE_FAULT_TEST=1 kernel disk
	$(call prepare_test_images,df,)
	$(call qemu_headless_for,df,3)
	grep -q "\[PANIC\] --- DOUBLE FAULT ---" debugcon-df.log
	grep -q "fault entered through dedicated TSS" debugcon-df.log

# `test-busybox-compat` — boot the unattended BusyBox compatibility runner as
#                         the initial process, then extract its on-disk report.
test-busybox-compat:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/bbcompat INIT_ARG0=bbcompat kernel disk
	$(call prepare_test_images,bbcompat,bbcompat.log)
	$(call qemu_headless_for,bbcompat,120)
	$(PYTHON) tools/dufs_extract.py dufs-bbcompat.img bbcompat.log bbcompat.log
	cat bbcompat.log
	grep -q "BBCOMPAT SUMMARY passed 255/255" bbcompat.log
	! grep -q "BBCOMPAT FAIL" bbcompat.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-bbcompat.log

# `test-linux-abi` — boot a static Linux/i386 ELF that checks syscall return
#                    values and errno-compatible negative results directly.
test-linux-abi:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/linuxabi INIT_ARG0=linuxabi kernel disk
	$(call prepare_test_images,linuxabi,linuxabi.log)
	$(call qemu_headless_for,linuxabi,30)
	$(PYTHON) tools/dufs_extract.py dufs-linuxabi.img linuxabi.log linuxabi.log
	cat linuxabi.log
	grep -q "LINUXABI SUMMARY passed 357/357" linuxabi.log
	! grep -q "LINUXABI FAIL" linuxabi.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-linuxabi.log

test-threadtest:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/threadtest INIT_ARG0=threadtest kernel disk
	$(call prepare_test_images,threadtest,threadtest.log)
	$(call qemu_headless_for,threadtest,30)
	$(PYTHON) tools/dufs_extract.py dufs-threadtest.img threadtest.log threadtest.log
	cat threadtest.log
	grep -q "THREADTEST PASS" threadtest.log
	! grep -q "THREADTEST FAIL" threadtest.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-threadtest.log

test-tcc:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/tcccompat INIT_ARG0=tcccompat kernel disk
	$(call prepare_test_images,tcc,tcc.log)
	$(call qemu_headless_for,tcc,120)
	$(PYTHON) tools/dufs_extract.py dufs-tcc.img tcc.log tcc.log
	cat tcc.log
	grep -q "TCCCOMPAT: version ok" tcc.log
	grep -q "TCCCOMPAT: compile ok" tcc.log
	grep -q "TCCCOMPAT: run ok" tcc.log
	grep -q "TCCCOMPAT: multi source write ok" tcc.log
	grep -q "TCCCOMPAT: multi compile ok" tcc.log
	grep -q "TCCCOMPAT: multi run ok" tcc.log
	grep -q "TCCCOMPAT: runtime source write ok" tcc.log
	grep -q "TCCCOMPAT: runtime compile ok" tcc.log
	grep -q "TCCCOMPAT: runtime run ok" tcc.log
	grep -q "TCCCOMPAT PASS" tcc.log
	! grep -q "TCCCOMPAT FAIL" tcc.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-tcc.log

# `test-ext3-linux-compat` — verify a freshly generated ext3 root with host
#                            e2fsprogs, then boot Drunix writable ext3 smoke
#                            tests and fsck the mutated root image.
test-ext3-linux-compat:
	$(MAKE) validate-ext3-linux
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/ext3wtest INIT_ARG0=ext3wtest kernel disk
	$(call prepare_test_images,ext3w,ext3wtest.log)
	$(call qemu_headless_for,ext3w,20)
	$(PYTHON) tools/dufs_extract.py dufs-ext3w.img ext3wtest.log ext3wtest.log
	cat ext3wtest.log
	grep -q "EXT3WTEST PASS" ext3wtest.log
	dd if=disk-ext3w.img of=disk-ext3w.fs bs=512 skip=$(PARTITION_START) count=$(FS_SECTORS) 2>/dev/null
	$(PYTHON) tools/check_ext3_linux_compat.py disk-ext3w.img
	$(PYTHON) tools/check_ext3_journal_activity.py disk-ext3w.img 1
	$(E2FSCK) -fn disk-ext3w.fs

# `test-ext3-host-write-interop` — use e2fsprogs debugfs to write into the
#                                  generated ext3 image, then read it back and
#                                  fsck the host-mutated image.
test-ext3-host-write-interop:
	$(MAKE) validate-ext3-linux
	rm -f disk-ext3-host.img disk-ext3-host.fs build/ext3-host.txt ext3-host-readback.txt
	mkdir -p build
	printf 'linux-host\n' > build/ext3-host.txt
	cp -f disk.fs disk-ext3-host.fs
	$(DEBUGFS) -w -R 'write build/ext3-host.txt linux-host.txt' disk-ext3-host.fs
	$(DEBUGFS) -R 'cat linux-host.txt' disk-ext3-host.fs > ext3-host-readback.txt
	grep -q '^linux-host$$' ext3-host-readback.txt
	$(PYTHON) tools/wrap_mbr.py disk-ext3-host.fs disk-ext3-host.img $(PARTITION_START) $(DISK_SECTORS) 0x83
	$(PYTHON) tools/check_ext3_linux_compat.py disk-ext3-host.img
	$(E2FSCK) -fn disk-ext3-host.fs

# `test-all` — run every test suite: in-kernel unit tests (KTEST, headless)
#              followed by all halt-inducing tests.  Exits non-zero if any
#              suite fails.
test-all:
	$(MAKE) test-headless
	$(MAKE) test-linux-abi
	$(MAKE) test-halt

# ─────────────────────────────────────────────────────────────────────────────
# UTILITY TARGETS
# ─────────────────────────────────────────────────────────────────────────────

# `all`     — default entry point: build the kernel ISO and disk image, then
#             boot the OS with the freshly built filesystem.
all: run-fresh

# `rebuild` — wipe all build outputs, rebuild the kernel and filesystem from
#             scratch, and boot.  Use this when you want a completely clean slate.
rebuild:
	$(MAKE) clean
	$(MAKE) run-fresh

# `clean`   — remove all build outputs: kernel objects, ELF, ISO, disk image,
#             generated docs, and dependency/sentinel files.
clean:
	find kernel -name '*.o' -delete
	find kernel -name '*.d' -delete
	$(RM) *.elf core.* disk.img dufs.img disk.fs dufs.fs disk-ext3w.fs disk-ext3-host.fs $(TEST_IMAGES) os.iso $(ISO_KERNEL) $(ISO_KERNEL_VGA) iso/boot/grub/grub.cfg "$(PDF)" "$(EPUB)" $(SENTINELS)
	$(RM) $(RUN_LOGS) $(TEST_LOGS) build/ext3-host.txt
	rm -rf build/busybox
	$(RM) docs/diagrams/*.png
	$(MAKE) -C user clean

.PHONY: all build kernel iso images disk fresh check \
        run run-stdio run-grub-menu run-fresh \
        debug debug-user debug-fresh \
        test test-fresh test-headless test-halt test-busybox-compat test-linux-abi test-threadtest test-tcc test-ext3-linux-compat test-ext3-host-write-interop test-all \
        validate-ext3-linux \
        pdf epub docs \
        rebuild clean

-include $(KOBJS:.o=.d) $(KTOBJS:.o=.d)

CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
NASM    := nasm
PYTHON  := python3
QEMU    := qemu-system-i386
GDB     := i386-elf-gdb
CFLAGS  := -m32 -g -ffreestanding -mno-sse -mno-sse2 -mno-mmx -msoft-float -Wstack-usage=1024
INC     := -I kernel -I kernel/arch -I kernel/mm -I kernel/drivers -I kernel/proc -I kernel/fs -I kernel/lib -I kernel/gui
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
           kernel/test/test_desktop.o
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
FORCE:

kernel/kernel.o: .ktest-flag
kernel/kernel.o: .double-fault-test-flag
kernel/kernel.o: .init-program-flag
kernel/lib/klog.o: .klog-debugcon-flag
kernel/drivers/mouse.o: .mouse-speed-flag
kernel/test/test_desktop.o: .mouse-speed-flag

# GRUB2 mkrescue (provided by: brew install i686-elf-grub xorriso)
GRUB_MKRESCUE := i686-elf-grub-mkrescue
ISO_KERNEL    := iso/boot/kernel.elf
DISK_SECTORS  := 102400
USER_PROGS    := shell chello hello writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello linuxhello linuxprobe linuxabi busybox bbcompat dufstest redirtest
USER_BINS     := $(addprefix user/,$(USER_PROGS))
DISK_FILES    := $(foreach prog,$(USER_PROGS),user/$(prog) bin/$(prog)) \
                 tools/hello.txt hello.txt \
                 tools/readme.txt readme.txt
QEMU_COMMON   := -drive format=raw,file=disk.img,if=ide,index=0 \
                 -drive format=raw,file=dufs.img,if=ide,index=1 \
                 -cdrom os.iso \
                 -boot d -no-reboot -no-shutdown \
                 -global isa-debugcon.iobase=0xe9
QEMU_LOGS     := -serial file:serial.log -debugcon file:debugcon.log

# ─── Pattern rules ───────────────────────────────────────────────────────────
%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

%.o: %.asm
	$(NASM) $< -f elf -o $@

# ─── Kernel link ─────────────────────────────────────────────────────────────
KOBJS = kernel/kernel-entry.o kernel/kernel.o \
        kernel/module.o kernel/module_exports.o \
        kernel/lib/klog.o \
        kernel/lib/kstring.o kernel/lib/kprintf.o kernel/lib/ksort.o \
        kernel/arch/gdt.o kernel/arch/gdt_flush.o \
        kernel/arch/idt.o kernel/arch/isr.o kernel/arch/sse.o kernel/arch/df_test.o \
        kernel/arch/irq.o kernel/arch/pit.o kernel/arch/clock.o \
        kernel/drivers/keyboard.o kernel/drivers/mouse.o kernel/drivers/ata.o \
        kernel/drivers/blkdev.o kernel/drivers/chardev.o kernel/drivers/tty.o \
        kernel/gui/display.o kernel/gui/framebuffer.o kernel/gui/font8x16.o kernel/gui/desktop.o kernel/gui/desktop_apps.o kernel/gui/terminal.o \
        kernel/mm/pmm.o kernel/mm/paging.o kernel/mm/paging_asm.o kernel/mm/fault.o kernel/mm/vma.o kernel/mm/kheap.o kernel/mm/slab.o \
        kernel/proc/elf.o kernel/proc/process.o kernel/proc/process_asm.o \
        kernel/proc/sched.o kernel/proc/syscall.o kernel/proc/core.o kernel/proc/mem_forensics.o kernel/proc/pipe.o kernel/proc/switch.o \
        kernel/proc/uaccess.o \
        kernel/fs/fs.o kernel/fs/vfs.o kernel/fs/procfs.o kernel/fs/ext3.o

$(KOBJS): .ktest-flag

kernel.elf: $(KOBJS) $(KTOBJS)
	$(LD) -m elf_i386 -o $@ -T kernel/kernel.ld $(KOBJS) $(KTOBJS)

# ─── User programs ───────────────────────────────────────────────────────────
# Declared phony so make always delegates to the user subdirectory's own
# dependency tracking — changes to user/*.c or user/lib/* are picked up
# without needing a manual clean.
.PHONY: $(USER_BINS)
$(USER_BINS):
	$(MAKE) -C user $(@F)

# ─── Hard-disk images ────────────────────────────────────────────────────────
# disk.img is the primary ATA master (hd0).  By default it is a deterministic
# ext3-compatible read-only root.  ROOT_FS=dufs builds hd0 as DUFS instead.
ifeq ($(ROOT_FS),dufs)
disk.img: $(USER_BINS) tools/hello.txt tools/readme.txt tools/mkfs.py
	$(PYTHON) tools/mkfs.py $@ $(DISK_SECTORS) $(DISK_FILES)
else
disk.img: $(USER_BINS) tools/hello.txt tools/readme.txt tools/mkext3.py
	$(PYTHON) tools/mkext3.py $@ $(DISK_SECTORS) $(DISK_FILES)
endif

# dufs.img is the primary ATA slave (hd1), mounted at /dufs during ext3-root
# boots.  It is intentionally not rebuilt by run-fresh when it already exists.
dufs.img: tools/mkfs.py
	$(PYTHON) tools/mkfs.py $@ $(DISK_SECTORS)

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
# images for tap-to-zoom). Prefer cairosvg, but fall back to rsvg-convert
# when the Python package is not installed.
docs/diagrams/%.png: docs/diagrams/%.svg
	@if $(PYTHON) -c "import cairosvg" >/dev/null 2>&1; then \
		$(PYTHON) -c "import cairosvg; cairosvg.svg2png(url='$<', write_to='$@', scale=2)"; \
	elif command -v rsvg-convert >/dev/null 2>&1; then \
		rsvg-convert -f png -z 2 -o "$@" "$<"; \
	else \
		echo "error: need Python package 'cairosvg' or the 'rsvg-convert' binary to build EPUB diagrams" >&2; \
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

os.iso: $(ISO_KERNEL) iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ iso

kernel: os.iso

# `disk`    — build the DUFS hard-disk image with all compiled user programs.
#             Use this to (re)populate the filesystem after adding/changing user
#             programs.  Most run/debug targets intentionally do NOT depend on
#             this so that the filesystem state is preserved across boots.
disk: disk.img dufs.img

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
	rm -f serial.log debugcon.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_LOGS)

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
	rm -f serial.log debugcon.log
	$(QEMU) -s -S \
	    $(QEMU_COMMON) \
	    $(QEMU_LOGS) &
	sleep 1
	$(GDB) -ex "set pagination off" \
	       -ex "set confirm off" \
	       -ex "set tcp auto-retry on" \
	       -ex "file kernel.elf" \
	       -ex "target remote localhost:1234" \
	       -ex "hbreak idt_init_early" \
	       -ex "continue"

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
	rm -f serial.log debugcon.log
	$(QEMU) -s -S \
	    $(QEMU_COMMON) \
	    $(QEMU_LOGS) &
	sleep 1
	$(GDB) -ex "set pagination off" \
	       -ex "set tcp auto-retry on" \
	       -ex "file kernel.elf" \
	       -ex "add-symbol-file user/$(APP) 0x400000" \
	       -ex "target remote localhost:1234" \
	       -ex "hbreak idt_init_early" \
	       -ex "continue"

# ─────────────────────────────────────────────────────────────────────────────
# RUN + FRESH FILESYSTEM  (rebuild disk.img before booting)
# ─────────────────────────────────────────────────────────────────────────────

# `run-fresh`   — rebuild the DUFS disk image from scratch, then boot the OS.
#                 Use this after adding or changing user programs to get a clean
#                 filesystem state.
run-fresh: os.iso disk.img dufs.img
	rm -f serial.log debugcon.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_LOGS)

# `debug-fresh` — rebuild the DUFS disk image from scratch, then boot paused
#                 under the GDB stub.  Combines `run-fresh` and `debug`.
debug-fresh: os.iso disk.img dufs.img
	rm -f serial.log debugcon.log
	$(QEMU) -s -S \
	    $(QEMU_COMMON) \
	    $(QEMU_LOGS) &
	sleep 1
	$(GDB) -ex "set pagination off" \
	       -ex "set confirm off" \
	       -ex "set tcp auto-retry on" \
	       -ex "file kernel.elf" \
	       -ex "target remote localhost:1234" \
	       -ex "hbreak idt_init_early" \
	       -ex "continue"

# ─────────────────────────────────────────────────────────────────────────────
# TEST TARGETS
# ─────────────────────────────────────────────────────────────────────────────

# `test`    — compile with KTEST=1 (in-kernel test suite) and boot under QEMU.
#             Tests run automatically at startup; results appear in debugcon.log.
#             QEMU stays open so you can inspect the shell after the suite runs.
#             Does NOT rebuild the filesystem.
test:
	$(MAKE) KTEST=1 run

# `test-fresh` — same as `test` but rebuilds the filesystem first.
#                Use when test cases rely on specific files being present on disk.
test-fresh:
	$(MAKE) KTEST=1 run-fresh

# `test-halt` — run halt-inducing kernel tests headlessly.  QEMU is launched
#               without a display and killed after a short timeout; the exit
#               status of the target reflects whether the expected panic fired.
#               Currently verifies the double-fault path via a dedicated TSS.
test-halt:
	$(MAKE) DOUBLE_FAULT_TEST=1 kernel disk
	rm -f serial-df.log debugcon-df.log disk-df.img dufs-df.img
	cp -f disk.img disk-df.img
	cp -f dufs.img dufs-df.img
	sh -c '$(QEMU) -display none -drive format=raw,file=disk-df.img,if=ide,index=0 -drive format=raw,file=dufs-df.img,if=ide,index=1 -cdrom os.iso -boot d -no-reboot -no-shutdown -serial file:serial-df.log -debugcon file:debugcon-df.log -global isa-debugcon.iobase=0xe9 >/dev/null 2>&1 & pid=$$!; sleep 3; kill $$pid >/dev/null 2>&1 || true; wait $$pid >/dev/null 2>&1 || true'
	grep -q "\[PANIC\] --- DOUBLE FAULT ---" debugcon-df.log
	grep -q "fault entered through dedicated TSS" debugcon-df.log

# `test-busybox-compat` — boot the unattended BusyBox compatibility runner as
#                         the initial process, then extract its on-disk report.
test-busybox-compat:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/bbcompat INIT_ARG0=bbcompat kernel disk
	rm -f serial-bbcompat.log debugcon-bbcompat.log disk-bbcompat.img dufs-bbcompat.img bbcompat.log
	cp -f disk.img disk-bbcompat.img
	cp -f dufs.img dufs-bbcompat.img
	sh -c '$(QEMU) -display none -drive format=raw,file=disk-bbcompat.img,if=ide,index=0 -drive format=raw,file=dufs-bbcompat.img,if=ide,index=1 -cdrom os.iso -boot d -no-reboot -no-shutdown -serial file:serial-bbcompat.log -debugcon file:debugcon-bbcompat.log -global isa-debugcon.iobase=0xe9 >/dev/null 2>&1 & pid=$$!; sleep 120; kill $$pid >/dev/null 2>&1 || true; wait $$pid >/dev/null 2>&1 || true'
	$(PYTHON) tools/dufs_extract.py dufs-bbcompat.img bbcompat.log bbcompat.log
	cat bbcompat.log
	grep -q "BBCOMPAT SUMMARY passed 255/255" bbcompat.log
	! grep -q "BBCOMPAT FAIL" bbcompat.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-bbcompat.log

# `test-linux-abi` — boot a static Linux/i386 ELF that checks syscall return
#                    values and errno-compatible negative results directly.
test-linux-abi:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/linuxabi INIT_ARG0=linuxabi kernel disk
	rm -f serial-linuxabi.log debugcon-linuxabi.log disk-linuxabi.img dufs-linuxabi.img linuxabi.log
	cp -f disk.img disk-linuxabi.img
	cp -f dufs.img dufs-linuxabi.img
	sh -c '$(QEMU) -display none -drive format=raw,file=disk-linuxabi.img,if=ide,index=0 -drive format=raw,file=dufs-linuxabi.img,if=ide,index=1 -cdrom os.iso -boot d -no-reboot -no-shutdown -serial file:serial-linuxabi.log -debugcon file:debugcon-linuxabi.log -global isa-debugcon.iobase=0xe9 >/dev/null 2>&1 & pid=$$!; sleep 30; kill $$pid >/dev/null 2>&1 || true; wait $$pid >/dev/null 2>&1 || true'
	$(PYTHON) tools/dufs_extract.py dufs-linuxabi.img linuxabi.log linuxabi.log
	cat linuxabi.log
	grep -q "LINUXABI SUMMARY passed 355/355" linuxabi.log
	! grep -q "LINUXABI FAIL" linuxabi.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-linuxabi.log

# `test-all` — run every test suite: in-kernel unit tests (KTEST) followed by
#              all halt-inducing tests.  Exits non-zero if any suite fails.
test-all:
	$(MAKE) test
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
	$(RM) *.elf core.* disk.img dufs.img disk-df.img dufs-df.img disk-bbcompat.img dufs-bbcompat.img disk-linuxabi.img dufs-linuxabi.img os.iso $(ISO_KERNEL) "$(PDF)" "$(EPUB)" .ktest-flag .double-fault-test-flag .klog-debugcon-flag .mouse-speed-flag .init-program-flag
	$(RM) -f serial-bbcompat.log debugcon-bbcompat.log bbcompat.log
	$(RM) -f serial-linuxabi.log debugcon-linuxabi.log linuxabi.log
	$(RM) -rf build/busybox
	$(RM) -f docs/diagrams/*.png
	$(MAKE) -C user clean

.PHONY: all kernel run run-stdio run-fresh disk \
        debug debug-user debug-fresh \
        test test-fresh test-halt test-busybox-compat test-linux-abi test-all \
        pdf epub docs \
        rebuild clean

-include $(KOBJS:.o=.d) $(KTOBJS:.o=.d)

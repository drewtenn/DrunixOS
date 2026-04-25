ARCH ?= x86

CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
NASM    := nasm
PYTHON  := python3
QEMU    := qemu-system-i386
GDB     := i386-elf-gdb
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck
SPARSE ?= sparse
SPARSEFLAGS ?= -Wno-non-pointer-null -nostdinc -I tools/sparse-include -I user/lib
SCAN_FAIL ?= 1
LINUX_I386_CC ?= i486-linux-musl-gcc
LINUX_I386_CROSS_COMPILE ?= i486-linux-musl-
LINUX_ARM64_CC ?= aarch64-linux-musl-gcc
LINUX_ARM64_CROSS_COMPILE ?= aarch64-linux-musl-
LINUX_CFLAGS ?= -static -Os -s
INCLUDE_BUSYBOX ?= 0
BUSYBOX_VERSION ?= 1.36.1
BUSYBOX_X86_BIN := build/busybox/x86/busybox
BUSYBOX_ARM64_BIN := build/busybox/arm64/busybox
BUSYBOX_ARM64_LDFLAGS ?= -Wl,-Ttext-segment=0x02000000
E2FSPROGS_SBIN ?= /opt/homebrew/opt/e2fsprogs/sbin
E2FSCK  ?= $(if $(wildcard $(E2FSPROGS_SBIN)/e2fsck),$(E2FSPROGS_SBIN)/e2fsck,e2fsck)
DUMPE2FS ?= $(if $(wildcard $(E2FSPROGS_SBIN)/dumpe2fs),$(E2FSPROGS_SBIN)/dumpe2fs,dumpe2fs)
DEBUGFS ?= $(if $(wildcard $(E2FSPROGS_SBIN)/debugfs),$(E2FSPROGS_SBIN)/debugfs,debugfs)
-include kernel/arch/$(ARCH)/arch.mk
CFLAGS  := -m32 -g -ffreestanding -mno-sse -mno-sse2 -mno-mmx -msoft-float -Wstack-usage=1024
INC     := -I kernel -I kernel/arch -I kernel/arch/$(ARCH) -I kernel/arch/$(ARCH)/mm -I kernel/platform/pc -I kernel/mm -I kernel/drivers -I kernel/blk -I kernel/proc -I kernel/fs -I kernel/lib -I kernel/gui
DEPFLAGS := -MMD -MP
MOUSE_SPEED ?= 4
ARM64_SMOKE_FALLBACK ?= 0
X86_SERIAL_CONSOLE ?= 0
ifeq ($(ARCH),arm64)
INIT_PROGRAM ?= bin/shell
INIT_ARG0 ?= shell
INIT_ENV0 ?= PATH=/usr/bin:/i686-linux-musl/bin:/bin
ROOT_FS ?= dufs
else
INIT_PROGRAM ?= bin/shell
INIT_ARG0 ?= shell
INIT_ENV0 ?= PATH=/usr/bin:/i686-linux-musl/bin:/bin
ROOT_FS ?= ext3
endif
CFLAGS += -DMOUSE_FRAMEBUFFER_PIXEL_SCALE=$(MOUSE_SPEED)
CFLAGS += -DDRUNIX_INIT_PROGRAM=\"$(INIT_PROGRAM)\"
CFLAGS += -DDRUNIX_INIT_ARG0=\"$(INIT_ARG0)\"
CFLAGS += -DDRUNIX_INIT_ENV0=\"$(INIT_ENV0)\"
CFLAGS += -DDRUNIX_ROOT_FS=\"$(ROOT_FS)\"
CFLAGS += -DDRUNIX_ARM64_SMOKE_FALLBACK=$(ARM64_SMOKE_FALLBACK)
ifeq ($(ARCH),x86)
ifeq ($(X86_SERIAL_CONSOLE),1)
CFLAGS += -DDRUNIX_X86_SERIAL_CONSOLE
endif
endif
ifeq ($(ARCH),arm64)
ARM_CFLAGS += -DDRUNIX_INIT_PROGRAM=\"$(INIT_PROGRAM)\"
ARM_CFLAGS += -DDRUNIX_INIT_ARG0=\"$(INIT_ARG0)\"
ARM_CFLAGS += -DDRUNIX_INIT_ENV0=\"$(INIT_ENV0)\"
ARM_CFLAGS += -DDRUNIX_ROOT_FS=\"$(ROOT_FS)\"
ARM_CFLAGS += -DDRUNIX_ARM64_SMOKE_FALLBACK=$(ARM64_SMOKE_FALLBACK)
ARM_CFLAGS += -DDRUNIX_ARM64_VGA=1
endif
NASMFLAGS :=

#Build with NO_DESKTOP = 1 to skip desktop init entirely and boot straight to
#the legacy console.The runtime "nodesktop" cmdline flag(set via grub) is
#still honored even when the desktop is compiled in.
ifneq ($(origin no_desktop),undefined)
NO_DESKTOP ?= $(no_desktop)
endif
NO_DESKTOP ?= 1
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
#Build with KTEST = 1 to compile the in - kernel test suite and run it at boot:
#make test(builds with tests enabled and launches QEMU)
KTEST ?= 0
ifeq ($(KTEST),1)
KLOG_TO_DEBUGCON ?= 1
CFLAGS += -DKTEST_ENABLED
INC    += -I kernel/test
ARM_CFLAGS += -DKTEST_ENABLED
ARM_INC    += -I kernel/test
include kernel/tests.mk
ARM_KTOBJS = $(KTOBJS:%.o=%.arm64.o)
else
KLOG_TO_DEBUGCON ?= 0
endif

DOUBLE_FAULT_TEST ?= 0
ifeq ($(DOUBLE_FAULT_TEST),1)
CFLAGS += -DDOUBLE_FAULT_TEST
endif

ifeq ($(KLOG_TO_DEBUGCON),1)
CFLAGS += -DKLOG_TO_DEBUGCON
endif

#Sentinel : recompile kernel.c whenever KTEST flips between 0 and 1.
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
.arm64-smoke-fallback-flag: FORCE
	echo "$(ARM64_SMOKE_FALLBACK)" | cmp -s - $@ || echo "$(ARM64_SMOKE_FALLBACK)" > $@
.x86-serial-console-flag: FORCE
	echo "$(X86_SERIAL_CONSOLE)" | cmp -s - $@ || echo "$(X86_SERIAL_CONSOLE)" > $@
.include-busybox-flag: FORCE
	echo "$(INCLUDE_BUSYBOX)" | cmp -s - $@ || echo "$(INCLUDE_BUSYBOX)" > $@
.no-desktop-flag: FORCE
	echo "$(NO_DESKTOP)" | cmp -s - $@ || echo "$(NO_DESKTOP)" > $@
.vga-text-flag: FORCE
	echo "$(VGA_TEXT)" | cmp -s - $@ || echo "$(VGA_TEXT)" > $@
.disk-sectors-flag: FORCE
	echo "$(DISK_SECTORS)" | cmp -s - $@ || echo "$(DISK_SECTORS)" > $@
#GRUB menu timeout(seconds).Default 0 boots the first entry instantly.
#Override(e.g.GRUB_TIMEOUT = 10) to display the menu — the `run - grub - menu`
#target uses this for interactive verification.
GRUB_TIMEOUT ?= 0
FORCE:

kernel/kernel.o: .ktest-flag FORCE
kernel/kernel.o: .double-fault-test-flag
kernel/kernel.o: .init-program-flag
kernel/kernel.o: .no-desktop-flag
kernel/kernel.o: .vga-text-flag
kernel/arch/x86/boot/kernel-entry.o: .vga-text-flag FORCE
kernel/lib/klog.o: .klog-debugcon-flag
kernel/platform/pc/mouse.o: .mouse-speed-flag
kernel/platform/pc/ata.o: .disk-sectors-flag
kernel/arch/x86/arch.o: .x86-serial-console-flag
kernel/test/test_desktop.o: .mouse-speed-flag

#GRUB2 mkrescue(provided by : brew install i686 - elf - grub xorriso)
GRUB_MKRESCUE := i686-elf-grub-mkrescue
ISO_KERNEL    := iso/boot/kernel.elf
ISO_KERNEL_VGA := iso/boot/kernel-vga.elf
DISK_SECTORS  ?= 262144
CFLAGS += -DDRUNIX_DISK_SECTORS=$(DISK_SECTORS)
PARTITION_START ?= 2048
FS_SECTORS     := $(shell expr $(DISK_SECTORS) - $(PARTITION_START))
include user/programs.mk
EXTRA_DISK_FILES ?=
BUSYBOX_DISK_FILES :=
BUSYBOX_DISK_DEPS :=
ifeq ($(INCLUDE_BUSYBOX),1)
ifeq ($(ARCH),x86)
BUSYBOX_DISK_FILES := $(BUSYBOX_X86_BIN) bin/busybox
BUSYBOX_DISK_DEPS := $(BUSYBOX_X86_BIN)
endif
endif
USER_PROGS    := $(PROGS)
USER_BINS     := $(addprefix user/,$(USER_PROGS))
DISK_FILES    := $(foreach prog,$(USER_PROGS),user/$(prog) bin/$(prog)) \
                 tools/hello.txt hello.txt \
                 tools/readme.txt readme.txt \
                 $(BUSYBOX_DISK_FILES) \
                 $(EXTRA_DISK_FILES)
LOG_DIR       := logs
IMG_DIR       := img
ROOT_DISK_IMG := $(IMG_DIR)/disk.img
DUFS_IMG      := $(IMG_DIR)/dufs.img
RUN_LOGS      := $(LOG_DIR)/serial.log $(LOG_DIR)/debugcon.log
TEST_SUFFIXES := ktest df ext3w threadtest
TEST_IMAGES   := $(foreach suffix,$(TEST_SUFFIXES),$(IMG_DIR)/disk-$(suffix).img $(IMG_DIR)/dufs-$(suffix).img) $(IMG_DIR)/disk-ext3-host.img
TEST_LOGS     := $(foreach suffix,$(TEST_SUFFIXES),$(LOG_DIR)/serial-$(suffix).log $(LOG_DIR)/debugcon-$(suffix).log) \
                 $(LOG_DIR)/ext3wtest.log \
                 $(LOG_DIR)/threadtest.log \
                 $(LOG_DIR)/ext3-host-readback.txt
SENTINELS     := .ktest-flag .double-fault-test-flag .klog-debugcon-flag \
                 .mouse-speed-flag .init-program-flag .no-desktop-flag \
                 .vga-text-flag .disk-sectors-flag .arm64-smoke-fallback-flag \
                 .x86-serial-console-flag .include-busybox-flag

QEMU_DISKS    = -drive format=raw,file=$(1),if=ide,index=0 \
                 -drive format=raw,file=$(2),if=ide,index=1
QEMU_BOOT     := -cdrom os.iso -boot d -no-reboot -no-shutdown \
                 -global isa-debugcon.iobase=0xe9
QEMU_COMMON   := $(call QEMU_DISKS,$(ROOT_DISK_IMG),$(DUFS_IMG)) $(QEMU_BOOT)
QEMU_LOGS     := -serial file:$(LOG_DIR)/serial.log -debugcon file:$(LOG_DIR)/debugcon.log
GDB_COMMON    := -ex "set pagination off" \
                 -ex "set confirm off" \
                 -ex "set tcp auto-retry on" \
                 -ex "file kernel.elf"

$(BUSYBOX_X86_BIN): tools/build_linux_busybox.sh Makefile
	env BUSYBOX_CC="$(LINUX_I386_CC)" BUSYBOX_CROSS_COMPILE="$(LINUX_I386_CROSS_COMPILE)" JOBS="$${JOBS:-4}" \
		tools/build_linux_busybox.sh $@ $(BUSYBOX_VERSION) build/busybox/x86-src

$(BUSYBOX_ARM64_BIN): tools/build_linux_busybox.sh Makefile
	env BUSYBOX_CC="$(LINUX_ARM64_CC)" BUSYBOX_CROSS_COMPILE="$(LINUX_ARM64_CROSS_COMPILE)" BUSYBOX_LDFLAGS="$(BUSYBOX_ARM64_LDFLAGS)" JOBS="$${JOBS:-4}" \
		tools/build_linux_busybox.sh $@ $(BUSYBOX_VERSION) build/busybox/arm64-src

define qemu_run
mkdir -p $(LOG_DIR)
rm -f $(RUN_LOGS)
$(QEMU) $(QEMU_COMMON) $(QEMU_LOGS)
endef

define qemu_debug
mkdir -p $(LOG_DIR)
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
mkdir -p $(LOG_DIR) $(IMG_DIR)
rm -f $(LOG_DIR)/serial-$(1).log $(LOG_DIR)/debugcon-$(1).log $(IMG_DIR)/disk-$(1).img $(IMG_DIR)/dufs-$(1).img $(2)
cp -f $(ROOT_DISK_IMG) $(IMG_DIR)/disk-$(1).img
cp -f $(DUFS_IMG) $(IMG_DIR)/dufs-$(1).img
endef

define qemu_headless_for
sh -c '$(QEMU) -display none $(call QEMU_DISKS,$(IMG_DIR)/disk-$(1).img,$(IMG_DIR)/dufs-$(1).img) $(QEMU_BOOT) -serial file:$(LOG_DIR)/serial-$(1).log -debugcon file:$(LOG_DIR)/debugcon-$(1).log >/dev/null 2>&1 & pid=$$!; sleep $(2); kill $$pid >/dev/null 2>&1 || true; wait $$pid >/dev/null 2>&1 || true'
endef

define qemu_headless_until_log
sh -c '$(QEMU) -display none $(call QEMU_DISKS,$(IMG_DIR)/disk-$(1).img,$(IMG_DIR)/dufs-$(1).img) $(QEMU_BOOT) -serial file:$(LOG_DIR)/serial-$(1).log -debugcon file:$(LOG_DIR)/debugcon-$(1).log >/dev/null 2>&1 & pid=$$!; for i in $$(seq 1 $(2)); do grep -q "$(3)" $(LOG_DIR)/debugcon-$(1).log 2>/dev/null && break; sleep 1; done; kill $$pid >/dev/null 2>&1 || true; wait $$pid >/dev/null 2>&1 || true'
endef

define require_tool
@command -v $(1) >/dev/null 2>&1 || { \
	echo "missing required tool: $(1)"; \
	echo "Install scanner dependencies from README.md, then rerun this target."; \
	exit 127; \
}
endef

# ─── Pattern rules ───────────────────────────────────────────────────────────
$(LOG_DIR) $(IMG_DIR):
	mkdir -p $@

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

%.o: %.asm
	$(NASM) $(NASMFLAGS) $< -f elf -o $@

include kernel/objects.mk

# ─── Kernel link ─────────────────────────────────────────────────────────────
$(KOBJS): .ktest-flag
kernel/test/%.o: CFLAGS += -Wno-stack-usage

kernel.elf: $(KOBJS) $(KTOBJS)
	$(LD) -m elf_i386 -o $@ -T kernel/arch/x86/linker.ld $(KOBJS) $(KTOBJS)

kernel/arch/x86/boot/kernel-entry-vga.o: kernel/arch/x86/boot/kernel-entry.asm
	$(NASM) -DDRUNIX_VGA_TEXT $< -f elf -o $@

kernel-vga.elf: $(KOBJS_VGA) $(KTOBJS)
	$(LD) -m elf_i386 -o $@ -T kernel/arch/x86/linker.ld $(KOBJS_VGA) $(KTOBJS)

# ─── User programs ───────────────────────────────────────────────────────────
# Declared phony so make always delegates to the user subdirectory's own
# dependency tracking — changes to user/*.c or user/lib/* are picked up
# without needing a manual clean.
.PHONY: $(USER_BINS)
$(USER_BINS): user/user.ld
	$(MAKE) -C user $(@F)

# Generated linker scripts.  user/user.ld.in is the single source of truth;
# the two arches differ only in load address.
user/user.ld: user/user.ld.in
	sed 's|@USER_LOAD_ADDR@|0x400000|' $< > $@

user/user_arm64.ld: user/user.ld.in
	sed 's|@USER_LOAD_ADDR@|0x02000000|' $< > $@

user/lib/libc.a user/lib/tcc_crt0.o:
	$(MAKE) -C user $(@F:%=lib/%)

# ─── Hard-disk images ────────────────────────────────────────────────────────
# disk.img is the primary ATA master (sda).  By default it is a deterministic
# Linux-compatible ext3 root partition.  ROOT_FS=dufs builds sda as DUFS
# instead.
ifeq ($(ROOT_FS),dufs)
disk.fs: $(USER_BINS) $(BUSYBOX_DISK_DEPS) tools/hello.txt tools/readme.txt tools/mkfs.py .disk-sectors-flag .include-busybox-flag
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS) $(DISK_FILES)
$(ROOT_DISK_IMG): disk.fs tools/wrap_mbr.py .disk-sectors-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA
else
disk.fs: $(USER_BINS) $(BUSYBOX_DISK_DEPS) tools/hello.txt tools/readme.txt tools/mkext3.py .disk-sectors-flag .include-busybox-flag
	$(PYTHON) tools/mkext3.py $@ $(FS_SECTORS) $(DISK_FILES)
$(ROOT_DISK_IMG): disk.fs tools/wrap_mbr.py .disk-sectors-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0x83
endif

# dufs.img is the primary ATA slave (sdb), mounted at /dufs during ext3-root
# boots.  It is intentionally not rebuilt by run-fresh when it already exists.
dufs.fs: tools/mkfs.py .disk-sectors-flag
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS)
$(DUFS_IMG): dufs.fs tools/wrap_mbr.py .disk-sectors-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py dufs.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA

disk.img: $(ROOT_DISK_IMG)
dufs.img: $(DUFS_IMG)

# ─── Documentation ───────────────────────────────────────────────────────────
include docs/build.mk

# ─────────────────────────────────────────────────────────────────────────────
# BUILD TARGETS  (compile/link only — do not launch QEMU)
# ─────────────────────────────────────────────────────────────────────────────
ifeq ($(ARCH),x86)

# `kernel`  — compile and link the kernel, then build the bootable GRUB ISO.
#             Does NOT touch img/disk.img.
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
disk: $(ROOT_DISK_IMG) $(DUFS_IMG)

build: kernel disk
iso: os.iso
images: disk
fresh: run-fresh
check: clang-tidy-include-check test-headless check-arch-boundary-reuse check-shared-shell-tests check-targets-generic check-test-wiring check-test-intent-coverage
check-phase6:
	python3 tools/test_kernel_arch_boundary_phase6.py

check-phase7:
	python3 tools/test_kernel_arch_boundary_phase7.py

check-shared-shell: check-shell-prompt check-user-programs check-sleep check-ctrl-c check-shell-history

check-shell-prompt:
	python3 tools/test_shell_prompt.py --arch x86

check-user-programs:
	python3 tools/test_user_programs.py --arch x86

check-sleep:
	python3 tools/test_sleep.py --arch x86

check-ctrl-c:
	python3 tools/test_ctrl_c.py --arch x86

check-shell-history:
	python3 tools/test_shell_history.py --arch x86

check-userspace-smoke:
	python3 tools/test_user_programs.py --arch x86

check-filesystem-init:
	python3 tools/test_shell_prompt.py --arch x86

check-kernel-unit: test-headless

check-syscall-parity:
	$(MAKE) ARCH=x86 test-headless

check-arch-boundary-reuse:
	python3 tools/test_arch_boundary_reuse.py

check-shared-shell-tests:
	python3 tools/test_shared_shell_tests_arch_neutral.py

check-busybox-compat:
	python3 tools/test_busybox_compat.py --arch x86

test-busybox-compat: check-busybox-compat

check-targets-generic:
	python3 tools/test_make_targets_arch_neutral.py

check-test-wiring:
	python3 tools/test_check_wiring.py --arch x86

check-test-intent-coverage:
	python3 tools/check_test_intent_coverage.py

validate-ext3-linux: $(ROOT_DISK_IMG) tools/check_ext3_linux_compat.py tools/check_ext3_journal_activity.py
	$(PYTHON) tools/check_ext3_linux_compat.py $(ROOT_DISK_IMG)
	$(E2FSCK) -fn disk.fs
	$(DUMPE2FS) -h disk.fs | grep -q 'Filesystem features:.*has_journal'
	$(DUMPE2FS) -h disk.fs | grep -q 'Journal inode:[[:space:]]*8'

# ─── Static analysis and style scans ─────────────────────────────────────────
SCAN_C_STYLE_SOURCES := $(shell find kernel -path kernel/test -prune -o -type f \( -name '*.c' -o -name '*.h' \) -print | sort) \
                        $(shell find user -type f \( -name '*.c' -o -name '*.h' \) | sort)
SCAN_KERNEL_C_SOURCES := $(shell find kernel -path kernel/test -prune -o -type f -name '*.c' -print | sort)
SCAN_USER_C_RUNTIME_OBJS := lib/cxx_init.o lib/syscall.o lib/malloc.o \
                            lib/string.o lib/ctype.o lib/stdlib.o \
                            lib/stdio.o lib/unistd.o lib/time.o

compile_commands.json: tools/generate_compile_commands.py kernel/objects.mk user/programs.mk user/Makefile Makefile
	$(PYTHON) tools/generate_compile_commands.py \
		--root=. \
		--output=$@ \
		--kernel-objs="$(KOBJS)" \
		--kernel-cc="$(CC)" \
		--kernel-cflags="$(CFLAGS)" \
		--kernel-inc="$(INC)" \
		--user-cc="$(CC)" \
		--user-cflags="-m32 -ffreestanding -nostdlib -fno-pie -no-pie -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall" \
		--linux-cc="$(LINUX_I386_CC)" \
		--linux-cflags="$(LINUX_CFLAGS)" \
		--user-c-runtime-objs="$(SCAN_USER_C_RUNTIME_OBJS)" \
		--user-c-progs="$(C_PROGS)" \
		--linux-c-progs="$(LINUX_C_PROGS)"

compile-commands: compile_commands.json

format-check:
	$(call require_tool,$(CLANG_FORMAT))
	@mkdir -p build
	@$(CLANG_FORMAT) --dry-run --Werror $(SCAN_C_STYLE_SOURCES) 2> build/clang-format.log || { \
		sed -n '1,120p' build/clang-format.log; \
		echo "... full clang-format report: build/clang-format.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

cppcheck: compile_commands.json
	$(call require_tool,$(CPPCHECK))
	@mkdir -p build/cppcheck
	@$(CPPCHECK) --project=compile_commands.json \
		--cppcheck-build-dir=build/cppcheck \
		--enable=warning \
		--std=c99 \
		--error-exitcode=1 > build/cppcheck.log 2>&1 || { \
		sed -n '1,180p' build/cppcheck.log; \
		echo "... full Cppcheck report: build/cppcheck.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

sparse-check:
	$(call require_tool,$(SPARSE))
	@mkdir -p build
	@: > build/sparse.log
	@rc=0; for src in $(SCAN_KERNEL_C_SOURCES); do \
		$(SPARSE) $(SPARSEFLAGS) $(CFLAGS) $(INC) $$src >> build/sparse.log 2>&1 || rc=1; \
	done; \
	if [ $$rc -ne 0 ] || [ -s build/sparse.log ]; then \
		sed -n '1,180p' build/sparse.log; \
		echo "... full Sparse report: build/sparse.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	fi

clang-tidy-include-check: compile_commands.json
	$(call require_tool,$(CLANG_TIDY))
	@mkdir -p build
	@$(PYTHON) tools/compile_commands_sources.py compile_commands.json --under kernel > build/clang-tidy-sources.txt
	@$(CLANG_TIDY) -p compile_commands.json --quiet \
		--checks=-*,misc-include-cleaner \
		$$(cat build/clang-tidy-sources.txt) > build/clang-tidy-include.log 2>&1 || { \
		sed -n '1,180p' build/clang-tidy-include.log; \
		echo "... full clang-tidy include report: build/clang-tidy-include.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

scan: format-check cppcheck clang-tidy-include-check sparse-check

# ─────────────────────────────────────────────────────────────────────────────
# RUN TARGETS  (boot QEMU with the current img/disk.img — no filesystem rebuild)
# ─────────────────────────────────────────────────────────────────────────────

# `run`     — boot the OS in QEMU.  Uses whatever img/disk.img is already on
#             disk; does NOT rebuild the filesystem.  Logs written to
#             logs/serial.log and logs/debugcon.log.
run: os.iso $(DUFS_IMG)
	$(call qemu_run)

# `run-grub-menu` — boot with the GRUB menu visible for 10 seconds so the
#                   menu entries can be exercised interactively.  Default
#                   builds keep timeout=0 (boots straight into the default).
run-grub-menu:
	$(MAKE) GRUB_TIMEOUT=10 run

# `run-stdio` — same as `run` but routes the debugcon port to the terminal
#               instead of a file.  Useful when you want to tail kernel log
#               output live without opening logs/debugcon.log separately.
run-stdio: os.iso $(DUFS_IMG) | $(LOG_DIR)
	$(QEMU) $(QEMU_COMMON) -serial file:$(LOG_DIR)/serial.log -debugcon stdio

# `debug`   — start QEMU paused with the GDB remote stub active, then attach
#             GDB loaded with kernel symbols. Uses a hardware breakpoint at
#             `idt_init_early` so the first stop lands at a point that is safe
#             for source-level stepping. Set breakpoints, then `continue` to
#             boot. Does NOT rebuild the filesystem.
#             Port: localhost:1234 (QEMU default).
debug: os.iso $(DUFS_IMG)
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
debug-user: os.iso $(DUFS_IMG)
	@test -n "$(APP)" || (echo "Usage: make debug-user APP=<program name>  (e.g. APP=shell)"; exit 1)
	$(call qemu_debug,-ex "add-symbol-file user/$(APP) 0x400000")

# ─────────────────────────────────────────────────────────────────────────────
# RUN + FRESH FILESYSTEM  (rebuild img/disk.img before booting)
# ─────────────────────────────────────────────────────────────────────────────

# `run-fresh`   — rebuild the root disk image from scratch, then boot the OS.
#                 Use this after adding or changing user programs to get a clean
#                 filesystem state.
run-fresh: $(ROOT_DISK_IMG)
	$(MAKE) run

# `debug-fresh` — rebuild the root disk image from scratch, then boot paused
#                 under the GDB stub.  Combines `run-fresh` and `debug`.
debug-fresh: $(ROOT_DISK_IMG)
	$(MAKE) debug

# ─── Test targets ────────────────────────────────────────────────────────────
include test/targets.mk

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
	$(RM) *.elf kernel8.img core.* disk.fs dufs.fs disk-ext3w.fs disk-ext3-host.fs $(ROOT_DISK_IMG) $(DUFS_IMG) $(TEST_IMAGES) os.iso $(ISO_KERNEL) $(ISO_KERNEL_VGA) iso/boot/grub/grub.cfg "$(PDF)" "$(EPUB)" $(SENTINELS) $(ARM_SERIAL_LOG)
	$(RM) build/arm64init.o build/crt0_arm64.o build/syscall_arm64.o build/arm64init.elf build/arm64-root.fs build/arm64-rootfs-empty
	$(RM) user/user.ld user/user_arm64.ld
	$(RM) $(RUN_LOGS) $(TEST_LOGS) build/ext3-host.txt
	rm -rf build/busybox
	rm -rf build/tcc
	rm -rf build/binutils
	rm -rf build/nano
	rm -rf build/gcc
	$(RM) docs/diagrams/*.png
	$(MAKE) -C user clean

.PHONY: all build kernel iso images disk fresh check \
        compile-commands format-check cppcheck sparse-check clang-tidy-include-check scan \
        disk.img dufs.img \
        run run-stdio run-grub-menu run-fresh \
        debug debug-user debug-fresh \
        test test-fresh test-headless test-halt test-threadtest test-ext3-linux-compat test-ext3-host-write-interop test-all test-busybox-compat \
        check-shared-shell check-shell-prompt check-user-programs check-sleep check-ctrl-c check-shell-history \
        check-phase6 check-phase7 check-userspace-smoke check-filesystem-init check-kernel-unit check-syscall-parity check-busybox-compat check-arch-boundary-reuse check-shared-shell-tests check-targets-generic check-test-wiring check-test-intent-coverage \
        validate-ext3-linux \
        pdf epub docs \
        rebuild clean
else

$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): CC = $(ARM_CC)
$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): CFLAGS = $(ARM_CFLAGS)
$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): INC = $(ARM_INC)

kernel/arch/arm64/%.o: kernel/arch/arm64/%.S
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) -c $< -o $@

kernel/lib/%.arm64.o: kernel/lib/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

kernel-arm64.elf: $(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_KTOBJS) Makefile kernel/arch/arm64/arch.mk
	$(ARM_LD) $(ARM_LDFLAGS) --wrap=syscall_case_exit_exit_group -o $@ $(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_KTOBJS)

kernel8.img: kernel-arm64.elf
	$(ARM_OBJCOPY) -O binary $< $@

kernel: kernel-arm64.elf

build: kernel-arm64.elf kernel8.img build/arm64-root.fs $(ARM_COMPILE_ONLY_OBJS)

kernel/arch/arm64/start_kernel.o: .init-program-flag .arm64-smoke-fallback-flag Makefile
kernel/arch/arm64/arch.o: Makefile
kernel/arch/arm64/video.o: Makefile
$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): .ktest-flag

iso: kernel8.img

images: kernel8.img

disk:
	@:

fresh: run

check: clang-tidy-include-check test-headless check-arch-boundary-reuse check-shared-shell-tests check-targets-generic check-test-wiring check-test-intent-coverage

test:
	$(MAKE) ARCH=$(ARCH) check

test-fresh:
	$(MAKE) ARCH=$(ARCH) test-headless

test-headless: check-kernel-unit check-shared-shell check-userspace-smoke check-filesystem-init check-syscall-parity

test-all:
	$(MAKE) ARCH=$(ARCH) check

compile-commands format-check cppcheck sparse-check clang-tidy-include-check scan:
	$(MAKE) ARCH=x86 $@

run-grub-menu run-stdio: run

debug debug-user debug-fresh test-halt test-threadtest test-ext3-linux-compat test-ext3-host-write-interop validate-ext3-linux:
	@echo "make ARCH=arm64 $@ is not implemented yet"
	@exit 2

check-shell-prompt:
	python3 tools/test_shell_prompt.py --arch arm64

check-user-programs:
	python3 tools/test_user_programs.py --arch arm64

check-shared-shell: check-shell-prompt check-user-programs check-sleep check-ctrl-c check-shell-history

check-phase6:
	python3 tools/test_kernel_arch_boundary_phase6.py

check-phase7:
	python3 tools/test_kernel_arch_boundary_phase7.py

check-userspace-smoke:
	python3 tools/test_arm64_userspace_smoke.py

check-filesystem-init:
	python3 tools/test_arm64_filesystem_init.py

check-kernel-unit:
	python3 tools/test_arm64_ktest.py

check-syscall-parity:
	python3 tools/test_arm64_syscall_parity.py

check-sleep:
	python3 tools/test_sleep.py --arch arm64

check-ctrl-c:
	python3 tools/test_ctrl_c.py --arch arm64

check-shell-history:
	python3 tools/test_shell_history.py --arch arm64

check-busybox-compat:
	python3 tools/test_busybox_compat.py --arch arm64

test-busybox-compat: check-busybox-compat

check-arch-boundary-reuse:
	python3 tools/test_arch_boundary_reuse.py

check-shared-shell-tests:
	python3 tools/test_shared_shell_tests_arch_neutral.py

check-targets-generic:
	python3 tools/test_make_targets_arch_neutral.py

check-test-wiring:
	python3 tools/test_check_wiring.py --arch arm64

check-test-intent-coverage:
	python3 tools/check_test_intent_coverage.py

run: kernel-arm64.elf | $(LOG_DIR)
	$(QEMU_ARM) -M $(QEMU_ARM_MACHINE) -kernel kernel-arm64.elf -serial null -serial stdio -device usb-kbd -monitor none -no-reboot

run-fresh: run

all: run-fresh

rebuild:
	$(MAKE) clean ARCH=$(ARCH)
	$(MAKE) run ARCH=$(ARCH)

clean:
	find kernel -name '*.o' -delete
	find kernel -name '*.d' -delete
	$(RM) *.elf kernel8.img core.* disk.fs dufs.fs disk-ext3w.fs disk-ext3-host.fs $(ROOT_DISK_IMG) $(DUFS_IMG) $(TEST_IMAGES) os.iso $(ISO_KERNEL) $(ISO_KERNEL_VGA) iso/boot/grub/grub.cfg "$(PDF)" "$(EPUB)" $(SENTINELS) $(ARM_SERIAL_LOG)
	$(RM) build/arm64init.o build/crt0_arm64.o build/syscall_arm64.o build/arm64init.elf build/arm64-root.fs build/arm64-rootfs-empty
	$(RM) user/user.ld user/user_arm64.ld
	$(RM) $(RUN_LOGS) $(TEST_LOGS) build/ext3-host.txt
	rm -rf build/busybox
	rm -rf build/tcc
	rm -rf build/binutils
	rm -rf build/nano
	rm -rf build/gcc
	$(RM) docs/diagrams/*.png
	$(MAKE) -C user clean

.PHONY: all build kernel iso images disk fresh check \
        compile-commands format-check cppcheck sparse-check clang-tidy-include-check scan \
        disk.img dufs.img \
        run run-stdio run-grub-menu run-fresh \
        debug debug-user debug-fresh \
        test test-fresh test-headless test-halt test-threadtest test-ext3-linux-compat test-ext3-host-write-interop test-all test-busybox-compat \
        check-shared-shell check-shell-prompt check-user-programs check-sleep check-ctrl-c check-shell-history \
        check-phase6 check-phase7 check-userspace-smoke check-filesystem-init check-kernel-unit check-syscall-parity check-busybox-compat check-arch-boundary-reuse check-shared-shell-tests check-targets-generic check-test-wiring check-test-intent-coverage \
        validate-ext3-linux \
        pdf epub docs \
        rebuild clean
endif

-include $(KOBJS:.o=.d) $(KTOBJS:.o=.d) $(ARM_KOBJS:.o=.d) $(ARM_SHARED_KOBJS:.o=.d) $(ARM_COMPILE_ONLY_OBJS:.o=.d) $(ARM_KTOBJS:.o=.d)

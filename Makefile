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
SPARSEFLAGS ?= -Wno-non-pointer-null -nostdinc -I tools/sparse-include -I user/runtime
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
BUILD_MODE ?= production
ifeq ($(BUILD_MODE),debug)
BUILD_OPT ?= -Og
else ifeq ($(BUILD_MODE),production)
BUILD_OPT ?= -O2
else
$(error BUILD_MODE must be production or debug)
endif
-include kernel/arch/$(ARCH)/arch.mk
CFLAGS  = -m32 -g $(BUILD_OPT) -ffreestanding -mno-sse -mno-sse2 -mno-mmx -msoft-float -Wstack-usage=1024 -Werror
INC     := -I kernel -I kernel/arch -I kernel/arch/$(ARCH) -I kernel/arch/$(ARCH)/boot -I kernel/arch/$(ARCH)/mm -I kernel/arch/$(ARCH)/proc -I kernel/arch/x86/platform/pc -I kernel/mm -I kernel/drivers -I kernel/blk -I kernel/proc -I kernel/fs -I kernel/lib -I kernel/gui -I shared
DEPFLAGS := -MMD -MP
MOUSE_SPEED ?= 4
ARM64_SMOKE_FALLBACK ?= 0
ARM64_HALT_TEST ?= 0
X86_SERIAL_CONSOLE ?= 0
X86_USER_LOAD_ADDR ?= 0x10000000

#Build with NO_DESKTOP = 1 to skip desktop init entirely and boot straight to
#the legacy console.The runtime "nodesktop" cmdline flag(set via grub) is
#still honored even when the desktop is compiled in.
ifneq ($(origin no_desktop),undefined)
NO_DESKTOP ?= $(no_desktop)
endif
NO_DESKTOP ?= 1

ifeq ($(NO_DESKTOP),1)
INIT_PROGRAM ?= bin/shell
INIT_ARG0 ?= shell
INIT_ENV0 ?= PATH=/bin
else
INIT_PROGRAM ?= bin/desktop
INIT_ARG0 ?= desktop
INIT_ENV0 ?= PATH=/usr/bin:/i686-linux-musl/bin:/bin
endif
ifeq ($(ARCH),arm64)
ROOT_FS ?= ext3
else
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
ARM_CFLAGS += -DDRUNIX_ARM64_HALT_TEST=$(ARM64_HALT_TEST)
ARM_CFLAGS += -DDRUNIX_ARM64_VGA=1
endif
NASMFLAGS := -Werror
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
INC    += -I kernel/test -I kernel/arch/$(ARCH)/test
ARM_CFLAGS += -DKTEST_ENABLED
ARM_INC    += -I kernel/test -I kernel/arch/arm64/test
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

#Sentinel : recompile startup code whenever KTEST flips between 0 and 1.
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
.arm64-halt-test-flag: FORCE
	echo "$(ARM64_HALT_TEST)" | cmp -s - $@ || echo "$(ARM64_HALT_TEST)" > $@
.x86-serial-console-flag: FORCE
	echo "$(X86_SERIAL_CONSOLE)" | cmp -s - $@ || echo "$(X86_SERIAL_CONSOLE)" > $@
.include-busybox-flag: FORCE
	echo "$(INCLUDE_BUSYBOX)" | cmp -s - $@ || echo "$(INCLUDE_BUSYBOX)" > $@
.disk-layout-flag: FORCE
	printf '%s\n%s\n' "$(ARCH)" "$(ROOT_FS)" | cmp -s - $@ || printf '%s\n%s\n' "$(ARCH)" "$(ROOT_FS)" > $@
.build-mode-flag: FORCE
	printf '%s\n%s\n' "$(BUILD_MODE)" "$(BUILD_OPT)" | cmp -s - $@ || printf '%s\n%s\n' "$(BUILD_MODE)" "$(BUILD_OPT)" > $@
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

kernel/arch/x86/start_kernel.o: .ktest-flag FORCE
kernel/arch/x86/start_kernel.o: .double-fault-test-flag
kernel/arch/x86/start_kernel.o: .init-program-flag
kernel/arch/x86/start_kernel.o: .no-desktop-flag
kernel/arch/x86/start_kernel.o: .vga-text-flag
kernel/arch/x86/boot/kernel-entry.o: .vga-text-flag FORCE
kernel/lib/klog.o: .klog-debugcon-flag
kernel/arch/x86/platform/pc/mouse.o: .mouse-speed-flag
kernel/arch/x86/platform/pc/ata.o: .disk-sectors-flag
kernel/arch/x86/arch.o: .x86-serial-console-flag
kernel/arch/x86/test/test_desktop.o: .mouse-speed-flag

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
USER_PROGS := $(PROGS)
USER_BUILD_ROOT := build/user/$(ARCH)
USER_BIN_DIR := $(USER_BUILD_ROOT)/bin
USER_BINS := $(addprefix $(USER_BIN_DIR)/,$(USER_PROGS))
# bin/desktop is the user-space compositor.  Even though it is listed in
# $(PROGS) the foreach below already drops it on the disk at /bin/desktop;
# no separate disk-files entry is required.
DISK_FILES := $(foreach prog,$(USER_PROGS),$(USER_BIN_DIR)/$(prog) bin/$(prog))
DISK_FILES += tools/hello.txt hello.txt
DISK_FILES += tools/readme.txt readme.txt
DISK_FILES += tools/wallpaper.jpg etc/wallpaper.jpg
DISK_FILES += $(BUSYBOX_DISK_FILES)
DISK_FILES += $(EXTRA_DISK_FILES)
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
                 .arm64-halt-test-flag .x86-serial-console-flag \
                 .include-busybox-flag .disk-layout-flag .build-mode-flag

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

debug debug-user debug-fresh: BUILD_MODE := debug
debug debug-user debug-fresh: BUILD_OPT := -Og

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

%.o: %.c .build-mode-flag
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

%.o: %.asm
	$(NASM) $(NASMFLAGS) $< -f elf -o $@

include kernel/objects.mk

# ─── Kernel link ─────────────────────────────────────────────────────────────
$(KOBJS) $(KOBJS_VGA) $(KTOBJS): .ktest-flag .build-mode-flag
kernel/test/%.o: CFLAGS += -Wno-stack-usage
kernel/arch/x86/test/%.o: CFLAGS += -Wno-stack-usage

kernel.elf: $(KOBJS) $(KTOBJS)
	$(LD) -m elf_i386 -o $@ -T kernel/arch/x86/linker.ld $(KOBJS) $(KTOBJS)

kernel/arch/x86/boot/kernel-entry-vga.o: kernel/arch/x86/boot/kernel-entry.asm
	$(NASM) $(NASMFLAGS) -DDRUNIX_VGA_TEXT $< -f elf -o $@

kernel-vga.elf: $(KOBJS_VGA) $(KTOBJS)
	$(LD) -m elf_i386 -o $@ -T kernel/arch/x86/linker.ld $(KOBJS_VGA) $(KTOBJS)

# ─── User programs ───────────────────────────────────────────────────────────
# Depend on FORCE so make always delegates to the user subdirectory's own
# dependency tracking while keeping concrete build outputs out of .PHONY.
ifneq ($(ARCH),arm64)
$(USER_BINS): FORCE
	$(MAKE) -C user USER_ARCH=$(ARCH) USER_LOAD_ADDR=$(X86_USER_LOAD_ADDR) BUILD_MODE=$(BUILD_MODE) BUILD_OPT=$(BUILD_OPT) ../$@
endif

include mk/disk-images.mk

# ─── Documentation ───────────────────────────────────────────────────────────
include docs/build.mk

# ─────────────────────────────────────────────────────────────────────────────
# BUILD TARGETS  (compile/link only — do not launch QEMU)
# ─────────────────────────────────────────────────────────────────────────────
ifeq ($(ARCH),x86)

include mk/run-x86.mk
include mk/checks.mk
include mk/scan-x86.mk
else

$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): CC = $(ARM_CC)
$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): CFLAGS = $(ARM_CFLAGS)
$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): INC = $(ARM_INC)
$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): .build-mode-flag

kernel/arch/arm64/%.o: kernel/arch/arm64/%.S
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) -c $< -o $@

kernel/lib/%.arm64.o: kernel/lib/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

kernel-arm64.elf: $(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_KTOBJS) Makefile kernel/arch/arm64/arch.mk
	$(ARM_LD) $(ARM_LDFLAGS) --wrap=syscall_case_exit_exit_group -o $@ $(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_KTOBJS)

kernel8.img: kernel-arm64.elf
	$(ARM_OBJCOPY) -O binary $< $@

kernel: kernel-arm64.elf

build: kernel-arm64.elf kernel8.img $(ROOT_DISK_IMG) $(ARM_BUILD_EXTRA) $(ARM_COMPILE_ONLY_OBJS)

kernel/arch/arm64/start_kernel.o: .init-program-flag .arm64-smoke-fallback-flag .arm64-halt-test-flag Makefile
kernel/arch/arm64/arch.o: Makefile
kernel/arch/arm64/platform/raspi3b/video.o: Makefile
$(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_COMPILE_ONLY_OBJS) $(ARM_KTOBJS): .ktest-flag

iso: kernel8.img

images: kernel8.img

disk: $(ROOT_DISK_IMG)

fresh: run

include mk/checks.mk
include mk/run-arm64.mk
include mk/scan-arm64.mk
endif

include mk/utility-targets.mk

-include $(KOBJS:.o=.d) $(KTOBJS:.o=.d) $(ARM_KOBJS:.o=.d) $(ARM_SHARED_KOBJS:.o=.d) $(ARM_COMPILE_ONLY_OBJS:.o=.d) $(ARM_KTOBJS:.o=.d)

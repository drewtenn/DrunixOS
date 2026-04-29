ARM_CC ?= aarch64-elf-gcc
ARM_CXX ?= aarch64-elf-g++
ARM_LD ?= aarch64-elf-ld
ARM_AR ?= aarch64-elf-ar
ARM_OBJCOPY ?= aarch64-elf-objcopy
ARM_GDB ?= aarch64-elf-gdb
ROOT_FS ?= ext3
BUILD_MODE ?= production

# Platform selector. Default raspi3b preserves existing behavior; virt is the
# QEMU `-M virt` machine added in 2026-04-29-gpu-h264-mvp.md Phase 1.
PLATFORM ?= raspi3b

ifneq ($(PLATFORM),raspi3b)
ifneq ($(PLATFORM),virt)
$(error PLATFORM must be raspi3b or virt; got "$(PLATFORM)")
endif
endif

ifeq ($(BUILD_MODE),debug)
BUILD_OPT ?= -Og
else ifeq ($(BUILD_MODE),production)
BUILD_OPT ?= -O2
else
$(error BUILD_MODE must be production or debug)
endif

include user/programs.mk

ARM_CFLAGS ?= -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align \
              -nostdlib -Wall -Wextra -Werror -g $(BUILD_OPT)

ifeq ($(PLATFORM),virt)
ARM_CFLAGS += -DDRUNIX_ARM64_PLATFORM_VIRT=1
ARM_LINKER_LD := kernel/arch/arm64/linker.virt.ld
else
ARM_CFLAGS += -DDRUNIX_ARM64_PLATFORM_RASPI3B=1
ARM_LINKER_LD := kernel/arch/arm64/linker.ld
endif

ARM_LDFLAGS ?= -nostdlib -T $(ARM_LINKER_LD)

ARM_PLATFORM_INC := -I kernel/arch/arm64/platform/$(PLATFORM)

ARM_INC := -I kernel -I kernel/lib -I kernel/arch -I kernel/arch/arm64 \
           -I kernel/arch/arm64/mm -I kernel/arch/arm64/proc \
           -I kernel/mm -I kernel/proc -I kernel/fs \
           -I kernel/drivers -I kernel/blk -I kernel/arch/arm64/platform \
           $(ARM_PLATFORM_INC) -I kernel/gui -I kernel/console \
           -I shared

# Per-platform object lists. raspi3b ships its full hardware backend; virt
# provides PL011 + stubs for M0 (irq/fb/usb/blk). M1+ replace the stubs.
ifeq ($(PLATFORM),virt)
ARM_PLATFORM_OBJS := kernel/arch/arm64/platform/virt/uart.o \
                     kernel/arch/arm64/platform/virt/stubs.o
else
ARM_PLATFORM_OBJS := kernel/arch/arm64/platform/raspi3b/uart.o \
                     kernel/arch/arm64/platform/raspi3b/irq.o \
                     kernel/arch/arm64/platform/raspi3b/video.o \
                     kernel/arch/arm64/platform/raspi3b/usb_hci.o \
                     kernel/arch/arm64/platform/raspi3b/emmc.o
endif

ARM_KOBJS := kernel/arch/arm64/boot.o \
             kernel/arch/arm64/arch.o \
             kernel/arch/arm64/exceptions.o \
             kernel/arch/arm64/exceptions_s.o \
             kernel/arch/arm64/proc/entry.o \
             kernel/arch/arm64/proc/arch_proc.o \
             kernel/arch/arm64/proc/smoke.o \
             kernel/arch/arm64/proc/smoke_blob.o \
             kernel/console/terminal.arm64.o \
             kernel/arch/arm64/mm/mmu.o \
             kernel/arch/arm64/mm/pmm.o \
             kernel/arch/arm64/mm/temp_map.o \
             kernel/mm/pmm_core.arm64.o \
             kernel/arch/arm64/timer.o \
             $(ARM_PLATFORM_OBJS) \
             kernel/arch/arm64/start_kernel.o \
             kernel/lib/kprintf.arm64.o \
             kernel/lib/kstring.arm64.o

ARM_BUILD_EXTRA :=
ifeq ($(ROOT_FS),dufs)
ARM_CFLAGS += -DDRUNIX_ARM64_EMBED_ROOTFS=1
ARM_BUILD_EXTRA += build/arm64-root.fs
ARM_KOBJS += kernel/arch/arm64/rootfs.o \
              kernel/arch/arm64/rootfs_blob.o
else
ARM_CFLAGS += -DDRUNIX_ARM64_EMBED_ROOTFS=0
endif

ARM_SHARED_KOBJS := kernel/lib/klog.arm64.o \
                    kernel/console/runtime.arm64.o \
                    kernel/mm/fault.arm64.o \
                    kernel/mm/vma.arm64.o \
                    kernel/mm/commit.arm64.o \
                    kernel/mm/kheap.arm64.o \
                    kernel/mm/slab.arm64.o \
                    kernel/drivers/blkdev.arm64.o \
                    kernel/drivers/blkdev_part.arm64.o \
                    kernel/blk/bcache.arm64.o \
                    kernel/drivers/chardev.arm64.o \
                    kernel/drivers/tty.arm64.o \
                    kernel/drivers/wmdev.arm64.o \
                    kernel/proc/elf.arm64.o \
                    kernel/arch/arm64/proc/elf64.arm64.o \
                    kernel/proc/process.arm64.o \
                    kernel/proc/resources.arm64.o \
                    kernel/proc/task_group.arm64.o \
                    kernel/proc/sched.arm64.o \
                    kernel/proc/uaccess.arm64.o \
                    kernel/arch/arm64/proc/syscall.arm64.o \
                    kernel/proc/syscall/helpers.arm64.o \
                    kernel/proc/syscall/console.arm64.o \
                    kernel/proc/syscall/task.arm64.o \
                    kernel/proc/syscall/tty.arm64.o \
                    kernel/arch/arm64/proc/core.arm64.o \
                    kernel/proc/mem_forensics.arm64.o \
                    kernel/proc/pipe.arm64.o \
                    kernel/proc/pty.arm64.o \
                    kernel/proc/init_launch.arm64.o \
                    kernel/fs/fs.arm64.o \
                    kernel/fs/vfs/core.arm64.o \
                    kernel/fs/vfs/lookup.arm64.o \
                    kernel/fs/vfs/mutation.arm64.o \
                    kernel/fs/ext3/main.arm64.o \
                    kernel/fs/ext3/blocks.arm64.o \
                    kernel/fs/ext3/lookup.arm64.o \
                    kernel/fs/ext3/mutation.arm64.o \
                    kernel/fs/ext3/journal.arm64.o \
                    kernel/fs/procfs.arm64.o \
                    kernel/fs/sysfs.arm64.o \
                    kernel/gui/display.arm64.o \
                    kernel/gui/framebuffer.arm64.o \
                    kernel/gui/font8x16.arm64.o \
                    kernel/console/fb_text_console.arm64.o

ARM_SHARED_KOBJS += kernel/proc/syscall/fd.arm64.o \
                    kernel/proc/syscall/fd_control.arm64.o \
                    kernel/proc/syscall/vfs/open.arm64.o \
                    kernel/proc/syscall/vfs/path.arm64.o \
                    kernel/proc/syscall/vfs/stat.arm64.o \
                    kernel/proc/syscall/vfs/dirents.arm64.o \
                    kernel/proc/syscall/vfs/mutation.arm64.o \
                    kernel/proc/syscall/time.arm64.o \
                    kernel/proc/syscall/info.arm64.o \
                    kernel/proc/syscall/mem.arm64.o \
                    kernel/proc/syscall/process.arm64.o \
                    kernel/proc/syscall/signal.arm64.o

ARM_COMPILE_ONLY_OBJS :=

ARM_USER_BUILD_DIR := build/user/arm64
ARM_USER_BIN_DIR := $(ARM_USER_BUILD_DIR)/bin
ARM_USER_OBJ_DIR := $(ARM_USER_BUILD_DIR)/obj
ARM_USER_RUNTIME_OBJ_DIR := $(ARM_USER_OBJ_DIR)/runtime
ARM_USER_APP_OBJ_DIR := $(ARM_USER_OBJ_DIR)/apps
ARM_USER_THIRD_PARTY_OBJ_DIR := $(ARM_USER_OBJ_DIR)/third_party
ARM_USER_NANOJPEG_OBJ_DIR := $(ARM_USER_THIRD_PARTY_OBJ_DIR)/nanojpeg
ARM_USER_RUNTIME_DIR := $(ARM_USER_BUILD_DIR)/runtime
ARM_USER_LINKER_DIR := $(ARM_USER_BUILD_DIR)/linker
ARM_USER_LINKER := $(ARM_USER_LINKER_DIR)/user.ld
ARM_USER_CFLAGS ?= -ffreestanding -nostdlib -fno-pic -fno-pie \
                   -fno-stack-protector -fno-omit-frame-pointer \
                   -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align -g $(BUILD_OPT) -Wall -Werror \
                   -fdebug-prefix-map=$(abspath .)=.
ARM_USER_CXXFLAGS ?= $(ARM_USER_CFLAGS) -fno-exceptions -fno-rtti \
                     -fno-use-cxa-atexit -fno-threadsafe-statics
ARM_USER_CXXLIBS := $(shell $(ARM_CXX) -print-libgcc-file-name 2>/dev/null)
ARM_USER_INCLUDES := -I user/apps -I user/runtime -I shared -I user/third_party/nanojpeg
ARM_USER_C_RUNTIME_OBJS := $(ARM_USER_RUNTIME_OBJ_DIR)/crt0.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/cxx_init.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/malloc.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/string.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/ctype.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/stdlib.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/stdio.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/unistd.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/time.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/drwin.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/drwin_gfx.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/desktop_font.o
ARM_USER_C_RUNTIME_LIB_OBJS := $(filter-out $(ARM_USER_RUNTIME_OBJ_DIR)/crt0.o,$(ARM_USER_C_RUNTIME_OBJS))
ARM_USER_CXX_RUNTIME_OBJS := $(ARM_USER_RUNTIME_OBJ_DIR)/cxxrt.o \
                             $(ARM_USER_RUNTIME_OBJ_DIR)/cxxabi.o
ARM_USER_C_GENERIC_PROGS := $(filter-out desktop,$(C_PROGS))
ARM_USER_C_BINS := $(addprefix $(ARM_USER_BIN_DIR)/,$(ARM_USER_C_GENERIC_PROGS))
ARM_USER_DESKTOP_BIN := $(ARM_USER_BIN_DIR)/desktop
ARM_USER_CXX_BINS := $(addprefix $(ARM_USER_BIN_DIR)/,$(CXX_PROGS))
ARM_USER_NATIVE_BINS := $(ARM_USER_C_BINS) $(ARM_USER_DESKTOP_BIN) $(ARM_USER_CXX_BINS)
ARM_EXTRA_ROOTFS_FILES ?=
ARM_BUSYBOX_ROOTFS_FILES :=
ARM_BUSYBOX_ROOTFS_DEPS :=
ifeq ($(INCLUDE_BUSYBOX),1)
ARM_BUSYBOX_ROOTFS_FILES := $(BUSYBOX_ARM64_BIN) bin/busybox
ARM_BUSYBOX_ROOTFS_DEPS := $(BUSYBOX_ARM64_BIN)
endif
ARM_USER_ROOTFS_FILES := $(foreach prog,$(C_PROGS) $(CXX_PROGS),$(ARM_USER_BIN_DIR)/$(prog) bin/$(prog)) \
                         build/arm64init.elf bin/arm64init \
                         tools/hello.txt hello.txt \
                         tools/readme.txt readme.txt \
                         $(ARM_BUSYBOX_ROOTFS_FILES) \
                         $(ARM_EXTRA_ROOTFS_FILES)

# Per-subdir compile rule: mechanical for each shared kernel subtree.
# Use a single template instantiated for each subdir.
define ARM_C_SUBDIR_RULE
kernel/$(1)/%.arm64.o: kernel/$(1)/%.c
	$$(ARM_CC) $$(ARM_CFLAGS) $$(DEPFLAGS) $$(ARM_INC) -c $$< -o $$@
endef

ARM_C_SUBDIRS := mm console gui blk drivers test fs fs/vfs fs/ext3 \
                 proc proc/syscall proc/syscall/vfs

$(foreach d,$(ARM_C_SUBDIRS),$(eval $(call ARM_C_SUBDIR_RULE,$(d))))

# arch/arm64/proc has both .arm64.o (elf64) and plain .o (smoke,
# arch_proc) targets, plus .S sources.
kernel/arch/arm64/proc/%.arm64.o: kernel/arch/arm64/proc/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/arch/arm64/test/%.arm64.o: kernel/arch/arm64/test/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/arch/arm64/proc/smoke.o: kernel/arch/arm64/proc/smoke.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/arch/arm64/proc/%.o: kernel/arch/arm64/proc/%.S
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

build/arm64-smoke-user.o: kernel/arch/arm64/proc/smoke_user.S .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/arm64-smoke-user.elf: build/arm64-smoke-user.o
	$(ARM_LD) -nostdlib -e _start -Ttext 0x200000 -o $@ $<

kernel/arch/arm64/proc/smoke_blob.o: kernel/arch/arm64/proc/smoke_blob.S build/arm64-smoke-user.elf
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

ARM64_SYSCALL_HEADERS := user/runtime/syscall_arm64.h \
                         user/runtime/syscall_arm64_asm.h \
                         user/runtime/syscall_arm64_nr.h \
                         user/runtime/ustrlen.h

build/arm64init.o: user/apps/arm64init.c $(ARM64_SYSCALL_HEADERS) .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -I user/apps -I user/runtime -c $< -o $@

build/crt0_arm64.o: user/runtime/crt0_arm64.S .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/syscall_arm64.o: user/runtime/syscall_arm64.c $(ARM64_SYSCALL_HEADERS) .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -I user/runtime -c $< -o $@

$(ARM_USER_LINKER): user/linker/user.ld.in Makefile
	@mkdir -p $(dir $@)
	sed 's|@USER_LOAD_ADDR@|0x02000000|' $< > $@

$(ARM_USER_RUNTIME_OBJ_DIR)/crt0.o: user/runtime/crt0_arm64.S .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -c $< -o $@

$(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o: user/runtime/syscall_arm64_compat.c user/runtime/syscall.h $(ARM64_SYSCALL_HEADERS) .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user/runtime -c $< -o $@

$(ARM_USER_RUNTIME_OBJ_DIR)/%.o: user/runtime/%.c .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) $(ARM_USER_INCLUDES) -c $< -o $@

$(ARM_USER_RUNTIME_OBJ_DIR)/%.o: user/runtime/%.cpp .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CXX) $(ARM_USER_CXXFLAGS) $(ARM_USER_INCLUDES) -c $< -o $@

$(ARM_USER_RUNTIME_OBJ_DIR)/desktop_font.o: user/apps/desktop_font.c user/apps/desktop_font.h .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) $(ARM_USER_INCLUDES) -c $< -o $@

$(ARM_USER_APP_OBJ_DIR)/%.o: user/apps/%.c .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) $(ARM_USER_INCLUDES) -c $< -o $@

$(ARM_USER_APP_OBJ_DIR)/%.o: user/apps/%.cpp .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CXX) $(ARM_USER_CXXFLAGS) $(ARM_USER_INCLUDES) -c $< -o $@

$(ARM_USER_RUNTIME_DIR)/libc.a: $(ARM_USER_C_RUNTIME_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(ARM_AR) rcs $@ $(ARM_USER_C_RUNTIME_LIB_OBJS)

$(ARM_USER_C_BINS): $(ARM_USER_BIN_DIR)/%: $(ARM_USER_APP_OBJ_DIR)/%.o $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_LINKER)
	@mkdir -p $(dir $@)
	$(ARM_LD) -nostdlib -T $(ARM_USER_LINKER) -o $@ $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_APP_OBJ_DIR)/$*.o

$(ARM_USER_CXX_BINS): $(ARM_USER_BIN_DIR)/%: $(ARM_USER_APP_OBJ_DIR)/%.o $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_CXX_RUNTIME_OBJS) $(ARM_USER_LINKER)
	@mkdir -p $(dir $@)
	$(ARM_LD) -nostdlib -T $(ARM_USER_LINKER) -o $@ $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_CXX_RUNTIME_OBJS) $(ARM_USER_APP_OBJ_DIR)/$*.o $(ARM_USER_CXXLIBS)

ARM_USER_DESKTOP_NANOJPEG_OBJS := $(ARM_USER_NANOJPEG_OBJ_DIR)/nanojpeg.o $(ARM_USER_NANOJPEG_OBJ_DIR)/nanojpeg_shim.o

$(ARM_USER_NANOJPEG_OBJ_DIR)/nanojpeg.o: user/third_party/nanojpeg/nanojpeg.c user/third_party/nanojpeg/nanojpeg.h .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user/runtime -DNJ_USE_LIBC=0 '-DNULL=((void*)0)' -Wno-unused-function -Wno-sign-compare -Wno-misleading-indentation -Wno-unused-parameter -Wno-implicit-fallthrough -c $< -o $@

$(ARM_USER_NANOJPEG_OBJ_DIR)/nanojpeg_shim.o: user/third_party/nanojpeg/nanojpeg_shim.c .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user/runtime -I user/third_party/nanojpeg -c $< -o $@

$(ARM_USER_APP_OBJ_DIR)/desktop_kbdmap.o: shared/kbdmap.c shared/kbdmap.h .build-mode-flag
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I shared -c $< -o $@

$(ARM_USER_BIN_DIR)/desktop: $(ARM_USER_APP_OBJ_DIR)/desktop.o $(ARM_USER_APP_OBJ_DIR)/desktop_kbdmap.o $(ARM_USER_DESKTOP_NANOJPEG_OBJS) $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_LINKER)
	@mkdir -p $(dir $@)
	$(ARM_LD) -nostdlib -T $(ARM_USER_LINKER) -o $@ $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_APP_OBJ_DIR)/desktop.o $(ARM_USER_APP_OBJ_DIR)/desktop_kbdmap.o $(ARM_USER_DESKTOP_NANOJPEG_OBJS)

build/arm64init.elf: build/crt0_arm64.o build/syscall_arm64.o build/arm64init.o $(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o $(ARM_USER_RUNTIME_OBJ_DIR)/cxx_init.o $(ARM_USER_LINKER) kernel/arch/arm64/arch.mk
	$(ARM_LD) -nostdlib -e _start -T $(ARM_USER_LINKER) -o $@ build/crt0_arm64.o $(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o build/syscall_arm64.o $(ARM_USER_RUNTIME_OBJ_DIR)/cxx_init.o build/arm64init.o

build/arm64-rootfs-empty:
	@mkdir -p $(dir $@)
	: > $@

build/arm64-root.fs: $(ARM_USER_NATIVE_BINS) build/arm64init.elf $(ARM_BUSYBOX_ROOTFS_DEPS) build/arm64-rootfs-empty tools/hello.txt tools/readme.txt tools/mkfs.py kernel/arch/arm64/arch.mk .include-busybox-flag
	$(PYTHON) tools/mkfs.py $@ 32768 \
		$(ARM_USER_ROOTFS_FILES) \
		build/arm64-rootfs-empty dev/.keep \
		build/arm64-rootfs-empty proc/.keep \
		build/arm64-rootfs-empty sys/.keep

kernel/arch/arm64/rootfs_blob.o: kernel/arch/arm64/rootfs_blob.S build/arm64-root.fs
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

QEMU_ARM ?= qemu-system-aarch64
QEMU_ARM_MACHINE ?= raspi3b
ARM_SERIAL_LOG ?= $(LOG_DIR)/serial-arm.log

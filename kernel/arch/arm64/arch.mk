ARM_CC ?= aarch64-elf-gcc
ARM_CXX ?= aarch64-elf-g++
ARM_LD ?= aarch64-elf-ld
ARM_AR ?= aarch64-elf-ar
ARM_OBJCOPY ?= aarch64-elf-objcopy

include user/programs.mk

ARM_CFLAGS ?= -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align \
              -nostdlib -Wall -Wextra -g -O2
ARM_LDFLAGS ?= -nostdlib -T kernel/arch/arm64/linker.ld
ARM_INC := -I kernel -I kernel/lib -I kernel/arch -I kernel/arch/arm64 \
           -I kernel/arch/arm64/mm -I kernel/mm -I kernel/proc -I kernel/fs \
           -I kernel/drivers -I kernel/blk -I kernel/platform/pc -I kernel/gui

ARM_KOBJS := kernel/arch/arm64/boot.o \
             kernel/arch/arm64/arch.o \
             kernel/arch/arm64/video.o \
             kernel/arch/arm64/exceptions.o \
             kernel/arch/arm64/exceptions_s.o \
             kernel/arch/arm64/irq.o \
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
             kernel/arch/arm64/uart.o \
             kernel/arch/arm64/usb_keyboard.o \
             kernel/arch/arm64/rootfs.o \
             kernel/arch/arm64/rootfs_blob.o \
             kernel/arch/arm64/start_kernel.o \
             kernel/lib/kprintf.arm64.o \
             kernel/lib/kstring.arm64.o

ARM_SHARED_KOBJS := kernel/lib/klog.arm64.o \
                    kernel/console/runtime.arm64.o \
                    kernel/mm/vma.arm64.o \
                    kernel/mm/kheap.arm64.o \
                    kernel/mm/slab.arm64.o \
                    kernel/drivers/blkdev.arm64.o \
                    kernel/drivers/blkdev_part.arm64.o \
                    kernel/blk/bcache.arm64.o \
                    kernel/drivers/chardev.arm64.o \
                    kernel/drivers/tty.arm64.o \
                    kernel/proc/elf.arm64.o \
                    kernel/arch/arm64/proc/elf64.arm64.o \
                    kernel/proc/process.arm64.o \
                    kernel/proc/resources.arm64.o \
                    kernel/proc/task_group.arm64.o \
                    kernel/proc/sched.arm64.o \
                    kernel/proc/uaccess.arm64.o \
                    kernel/proc/syscall.arm64.o \
                    kernel/proc/syscall/helpers.arm64.o \
                    kernel/proc/syscall/console.arm64.o \
                    kernel/proc/syscall/task.arm64.o \
                    kernel/proc/syscall/tty.arm64.o \
                    kernel/proc/core.arm64.o \
                    kernel/proc/mem_forensics.arm64.o \
                    kernel/proc/pipe.arm64.o \
                    kernel/proc/init_launch.arm64.o \
                    kernel/fs/fs.arm64.o \
                    kernel/fs/vfs/core.arm64.o \
                    kernel/fs/vfs/lookup.arm64.o \
                    kernel/fs/vfs/mutation.arm64.o \
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

ARM_USER_BUILD_DIR := build/arm64-user
ARM_USER_CFLAGS ?= -ffreestanding -nostdlib -fno-pic -fno-pie \
                   -fno-stack-protector -fno-omit-frame-pointer \
                   -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align -g -Og -Wall \
                   -fdebug-prefix-map=$(abspath .)=.
ARM_USER_CXXFLAGS ?= $(ARM_USER_CFLAGS) -fno-exceptions -fno-rtti \
                     -fno-use-cxa-atexit -fno-threadsafe-statics
ARM_USER_CXXLIBS := $(shell $(ARM_CXX) -print-libgcc-file-name 2>/dev/null)
ARM_USER_C_RUNTIME_OBJS := $(ARM_USER_BUILD_DIR)/lib/crt0.o \
                           $(ARM_USER_BUILD_DIR)/lib/cxx_init.o \
                           $(ARM_USER_BUILD_DIR)/lib/syscall.o \
                           $(ARM_USER_BUILD_DIR)/lib/malloc.o \
                           $(ARM_USER_BUILD_DIR)/lib/string.o \
                           $(ARM_USER_BUILD_DIR)/lib/ctype.o \
                           $(ARM_USER_BUILD_DIR)/lib/stdlib.o \
                           $(ARM_USER_BUILD_DIR)/lib/stdio.o \
                           $(ARM_USER_BUILD_DIR)/lib/unistd.o \
                           $(ARM_USER_BUILD_DIR)/lib/time.o
ARM_USER_C_RUNTIME_LIB_OBJS := $(filter-out $(ARM_USER_BUILD_DIR)/lib/crt0.o,$(ARM_USER_C_RUNTIME_OBJS))
ARM_USER_CXX_RUNTIME_OBJS := $(ARM_USER_BUILD_DIR)/lib/cxxrt.o \
                             $(ARM_USER_BUILD_DIR)/lib/cxxabi.o
ARM_USER_C_BINS := $(addprefix $(ARM_USER_BUILD_DIR)/,$(C_PROGS))
ARM_USER_CXX_BINS := $(addprefix $(ARM_USER_BUILD_DIR)/,$(CXX_PROGS))
ARM_USER_NATIVE_BINS := $(ARM_USER_C_BINS) $(ARM_USER_CXX_BINS)
ARM_USER_ROOTFS_FILES := $(foreach prog,$(C_PROGS) $(CXX_PROGS),$(ARM_USER_BUILD_DIR)/$(prog) bin/$(prog)) \
                         build/arm64init.elf bin/arm64init

kernel/mm/%.arm64.o: kernel/mm/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/console/%.arm64.o: kernel/console/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/gui/%.arm64.o: kernel/gui/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/blk/%.arm64.o: kernel/blk/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/drivers/%.arm64.o: kernel/drivers/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/test/%.arm64.o: kernel/test/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/fs/%.arm64.o: kernel/fs/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/fs/vfs/%.arm64.o: kernel/fs/vfs/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/proc/%.arm64.o: kernel/proc/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/proc/syscall/%.arm64.o: kernel/proc/syscall/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/proc/syscall/vfs/%.arm64.o: kernel/proc/syscall/vfs/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/arch/arm64/proc/%.arm64.o: kernel/arch/arm64/proc/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/arch/arm64/proc/smoke.o: kernel/arch/arm64/proc/smoke.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/arch/arm64/proc/%.o: kernel/arch/arm64/proc/%.S
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

build/arm64-smoke-user.o: kernel/arch/arm64/proc/smoke_user.S
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/arm64-smoke-user.elf: build/arm64-smoke-user.o
	$(ARM_LD) -nostdlib -e _start -Ttext 0x200000 -o $@ $<

kernel/arch/arm64/proc/smoke_blob.o: kernel/arch/arm64/proc/smoke_blob.S build/arm64-smoke-user.elf
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

build/arm64init.o: user/arm64init.c user/lib/syscall_arm64.h
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -I user/lib -c $< -o $@

build/crt0_arm64.o: user/lib/crt0_arm64.S
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/syscall_arm64.o: user/lib/syscall_arm64.c user/lib/syscall_arm64.h
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -I user/lib -c $< -o $@

$(ARM_USER_BUILD_DIR)/lib/crt0.o: user/lib/crt0_arm64.S
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -c $< -o $@

$(ARM_USER_BUILD_DIR)/lib/syscall.o: user/lib/syscall_arm64_compat.c user/lib/syscall.h
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user -I user/lib -c $< -o $@

$(ARM_USER_BUILD_DIR)/lib/%.o: user/lib/%.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user -I user/lib -c $< -o $@

$(ARM_USER_BUILD_DIR)/lib/%.o: user/lib/%.cpp
	@mkdir -p $(dir $@)
	$(ARM_CXX) $(ARM_USER_CXXFLAGS) -I user -I user/lib -c $< -o $@

$(ARM_USER_BUILD_DIR)/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user -I user/lib -c $< -o $@

$(ARM_USER_BUILD_DIR)/%.o: user/%.cpp
	@mkdir -p $(dir $@)
	$(ARM_CXX) $(ARM_USER_CXXFLAGS) -I user -I user/lib -c $< -o $@

$(ARM_USER_BUILD_DIR)/lib/libc.a: $(ARM_USER_C_RUNTIME_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(ARM_AR) rcs $@ $(ARM_USER_C_RUNTIME_LIB_OBJS)

$(ARM_USER_C_BINS): $(ARM_USER_BUILD_DIR)/%: $(ARM_USER_BUILD_DIR)/%.o $(ARM_USER_C_RUNTIME_OBJS) user/user_arm64.ld
	$(ARM_LD) -nostdlib -T user/user_arm64.ld -o $@ $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_BUILD_DIR)/$*.o

$(ARM_USER_CXX_BINS): $(ARM_USER_BUILD_DIR)/%: $(ARM_USER_BUILD_DIR)/%.o $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_CXX_RUNTIME_OBJS) user/user_arm64.ld
	$(ARM_LD) -nostdlib -T user/user_arm64.ld -o $@ $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_CXX_RUNTIME_OBJS) $(ARM_USER_BUILD_DIR)/$*.o $(ARM_USER_CXXLIBS)

build/arm64init.elf: build/crt0_arm64.o build/syscall_arm64.o build/arm64init.o $(ARM_USER_BUILD_DIR)/lib/syscall.o $(ARM_USER_BUILD_DIR)/lib/cxx_init.o kernel/arch/arm64/arch.mk
	$(ARM_LD) -nostdlib -e _start -T user/user_arm64.ld -o $@ build/crt0_arm64.o $(ARM_USER_BUILD_DIR)/lib/syscall.o build/syscall_arm64.o $(ARM_USER_BUILD_DIR)/lib/cxx_init.o build/arm64init.o

build/arm64-rootfs-empty:
	@mkdir -p $(dir $@)
	: > $@

build/arm64-root.fs: $(ARM_USER_NATIVE_BINS) build/arm64init.elf build/arm64-rootfs-empty tools/mkfs.py kernel/arch/arm64/arch.mk
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

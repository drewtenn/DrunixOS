ARM_CC ?= aarch64-elf-gcc
ARM_LD ?= aarch64-elf-ld
ARM_OBJCOPY ?= aarch64-elf-objcopy

ARM_CFLAGS ?= -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mcpu=cortex-a53 -mgeneral-regs-only \
              -nostdlib -Wall -Wextra -g -O2
ARM_LDFLAGS ?= -nostdlib -T kernel/arch/arm64/linker.ld

ARM_KOBJS := kernel/arch/arm64/boot.o \
             kernel/arch/arm64/arch.o \
             kernel/arch/arm64/exceptions.o \
             kernel/arch/arm64/exceptions_s.o \
             kernel/arch/arm64/irq.o \
             kernel/arch/arm64/proc/arch_proc.o \
             kernel/console/terminal.arm64.o \
             kernel/arch/arm64/mm/mmu.o \
             kernel/arch/arm64/mm/pmm.o \
             kernel/arch/arm64/mm/temp_map.o \
             kernel/mm/pmm_core.arm64.o \
             kernel/arch/arm64/timer.o \
             kernel/arch/arm64/uart.o \
             kernel/arch/arm64/start_kernel.o \
             kernel/lib/kprintf.arm64.o \
             kernel/lib/kstring.arm64.o

ARM_COMPILE_ONLY_OBJS := kernel/arch/arm64/proc/elf64.arm64.o

kernel/mm/%.arm64.o: kernel/mm/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) -I kernel -I kernel/lib -I kernel/arch -I kernel/arch/arm64 -I kernel/mm -I kernel/proc -I kernel/fs -I kernel/drivers -I kernel/blk -c $< -o $@

kernel/console/%.arm64.o: kernel/console/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) -I kernel -I kernel/lib -I kernel/arch -I kernel/arch/arm64 -I kernel/mm -I kernel/proc -I kernel/fs -I kernel/drivers -I kernel/blk -c $< -o $@

kernel/arch/arm64/proc/%.arm64.o: kernel/arch/arm64/proc/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) -I kernel -I kernel/lib -I kernel/arch -I kernel/arch/arm64 -I kernel/mm -I kernel/proc -I kernel/fs -I kernel/drivers -I kernel/blk -c $< -o $@

QEMU_ARM ?= qemu-system-aarch64
QEMU_ARM_MACHINE ?= raspi3b
ARM_SERIAL_LOG ?= $(LOG_DIR)/serial-arm.log

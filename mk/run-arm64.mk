# arm64 run, debug, and arch-specific test entry points.
test:
	$(MAKE) ARCH=$(ARCH) check

test-fresh:
	$(MAKE) ARCH=$(ARCH) test-headless

test-headless: check-kernel-unit check-shared-shell check-userspace-smoke check-filesystem-init check-syscall-parity

test-all: test-headless test-halt test-threadtest

run-grub-menu run-stdio: run

run: kernel-arm64.elf $(ROOT_DISK_IMG) | $(LOG_DIR)
	$(QEMU_ARM) -M $(QEMU_ARM_MACHINE) -kernel kernel-arm64.elf -drive if=sd,format=raw,file=$(ROOT_DISK_IMG) -serial null -serial stdio -device usb-kbd -monitor none -no-reboot

run-fresh: run

ARM_GDB_COMMON = -ex "set pagination off" \
                 -ex "set confirm off" \
                 -ex "set tcp auto-retry on" \
                 -ex "file kernel-arm64.elf"

define arm64_qemu_debug
mkdir -p $(LOG_DIR)
rm -f $(ARM_SERIAL_LOG)
$(QEMU_ARM) -M $(QEMU_ARM_MACHINE) -kernel kernel-arm64.elf \
    -drive if=sd,format=raw,file=$(ROOT_DISK_IMG) \
    -serial null -serial file:$(ARM_SERIAL_LOG) -device usb-kbd \
    -monitor none -no-reboot -s -S &
sleep 1
$(ARM_GDB) $(ARM_GDB_COMMON) $(1) \
       -ex "target remote localhost:1234" \
       -ex "hbreak arm64_start_kernel" \
       -ex "continue"
endef

debug: kernel-arm64.elf
	$(call arm64_qemu_debug,)

debug-user: kernel-arm64.elf $(ROOT_DISK_IMG)
	@test -n "$(APP)" || (echo "Usage: make debug-user APP=<program name>  (e.g. APP=shell)"; exit 1)
	$(call arm64_qemu_debug,-ex "add-symbol-file $(ARM_USER_BIN_DIR)/$(APP) 0x02000000")

debug-fresh: $(ROOT_DISK_IMG)
	$(MAKE) ARCH=$(ARCH) BUILD_MODE=$(BUILD_MODE) BUILD_OPT=$(BUILD_OPT) debug

test-halt:
	$(PYTHON) tools/test_arm64_halt.py

test-threadtest:
	$(PYTHON) tools/test_arm64_threadtest.py

test-desktop-screenshot:
	@echo "desktop screenshot test is x86-only until the ARM64 desktop exists"

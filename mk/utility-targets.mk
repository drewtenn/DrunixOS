all: run-fresh

ifeq ($(ARCH),arm64)
rebuild:
	$(MAKE) clean ARCH=$(ARCH)
	$(MAKE) run ARCH=$(ARCH)
else
rebuild:
	$(MAKE) clean
	$(MAKE) run-fresh
endif

clean:
	find kernel -name '*.o' -delete
	find kernel -name '*.d' -delete
	$(RM) *.elf kernel8.img core.* disk.fs dufs.fs disk-ext3w.fs disk-ext3-host.fs $(ROOT_DISK_IMG) $(DUFS_IMG) $(TEST_IMAGES) os.iso $(ISO_KERNEL) $(ISO_KERNEL_VGA) iso/boot/grub/grub.cfg "$(PDF)" "$(EPUB)" $(SENTINELS) $(ARM_SERIAL_LOG)
	$(RM) build/arm64init.o build/crt0_arm64.o build/syscall_arm64.o build/arm64init.elf build/arm64-root.fs build/arm64-rootfs-empty
	rm -rf build/user
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
        test test-fresh test-headless test-halt test-threadtest test-desktop-screenshot test-ext3-linux-compat test-ext3-host-write-interop test-all test-busybox-compat \
        check-shared-shell check-shell-prompt check-user-programs check-sleep check-ctrl-c check-shell-history check-user-runtime-string \
        check-phase6 check-phase7 check-userspace-smoke check-filesystem-init check-kernel-unit check-syscall-parity check-busybox-compat check-arch-boundary-reuse check-start-boundary check-platform-split check-dev-loop-parity check-ext3-root-parity check-shared-shell-tests check-targets-generic check-makefile-decomposition check-warning-policy check-test-wiring check-test-intent-coverage \
        validate-ext3-linux \
        pdf epub docs \
        rebuild clean

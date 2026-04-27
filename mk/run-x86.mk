# x86 build, run, debug, and test entry points.
$(ISO_KERNEL): kernel.elf
	cp $< $@

$(ISO_KERNEL_VGA): kernel-vga.elf
	cp $< $@

# Regenerate grub.cfg only when the rendered output differs from what's on disk.
iso/boot/grub/grub.cfg: iso/boot/grub/grub.cfg.in FORCE
	@sed 's/@TIMEOUT@/$(GRUB_TIMEOUT)/' $< | cmp -s - $@ 2>/dev/null || \
		sed 's/@TIMEOUT@/$(GRUB_TIMEOUT)/' $< > $@

os.iso: $(ISO_KERNEL) $(ISO_KERNEL_VGA) iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ iso

kernel: os.iso
disk: $(ROOT_DISK_IMG) $(DUFS_IMG)
build: kernel disk
iso: os.iso
images: disk
fresh: run-fresh

# `run` boots QEMU with the current img/disk.img; it does not rebuild the root
# filesystem, so state persists across boots.
run: os.iso $(DUFS_IMG)
	$(call qemu_run)

run-grub-menu:
	$(MAKE) GRUB_TIMEOUT=10 run

run-stdio: os.iso $(DUFS_IMG) | $(LOG_DIR)
	$(QEMU) $(QEMU_COMMON) -serial file:$(LOG_DIR)/serial.log -debugcon stdio

debug: os.iso $(DUFS_IMG)
	$(call qemu_debug,)

ifneq ($(APP),)
debug-user: $(USER_BIN_DIR)/$(APP)
endif

debug-user: os.iso $(DUFS_IMG)
	@test -n "$(APP)" || (echo "Usage: make debug-user APP=<program name>  (e.g. APP=shell)"; exit 1)
	$(call qemu_debug,-ex "add-symbol-file $(USER_BIN_DIR)/$(APP) $(X86_USER_LOAD_ADDR)")

run-fresh: $(ROOT_DISK_IMG)
	$(MAKE) run

debug-fresh: $(ROOT_DISK_IMG)
	$(MAKE) BUILD_MODE=$(BUILD_MODE) BUILD_OPT=$(BUILD_OPT) debug

include test/targets.mk

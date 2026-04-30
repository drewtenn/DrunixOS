# Hard-disk images. disk.img is the primary root disk: ATA master on x86,
# SD media for the Pi EMMC controller on arm64 raspi3b, and a virtio-blk
# device on arm64 virt (M2.4c reuses the same MBR-wrapped image; the raw
# partition layout is identical). ROOT_FS=dufs builds sda as DUFS; the
# default builds a deterministic Linux-compatible ext3 root partition.
ifeq ($(ARCH),arm64)
ifeq ($(ROOT_FS),dufs)
disk.fs: $(ARM_USER_NATIVE_BINS) build/arm64init.elf $(ARM_BUSYBOX_ROOTFS_DEPS) tools/hello.txt tools/readme.txt tools/wallpaper.jpg tools/mkfs.py .disk-sectors-flag .include-busybox-flag .disk-layout-flag
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS) $(ARM_USER_ROOTFS_FILES)
$(ROOT_DISK_IMG): disk.fs tools/wrap_mbr.py .disk-sectors-flag .disk-layout-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA
else
disk.fs: $(ARM_USER_NATIVE_BINS) build/arm64init.elf $(ARM_BUSYBOX_ROOTFS_DEPS) tools/hello.txt tools/readme.txt tools/wallpaper.jpg tools/mkext3.py .disk-sectors-flag .include-busybox-flag .disk-layout-flag
	$(PYTHON) tools/mkext3.py $@ $(FS_SECTORS) $(ARM_USER_ROOTFS_FILES)
$(ROOT_DISK_IMG): disk.fs tools/wrap_mbr.py .disk-sectors-flag .disk-layout-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0x83
endif
else
ifeq ($(ROOT_FS),dufs)
disk.fs: $(USER_BINS) $(BUSYBOX_DISK_DEPS) tools/hello.txt tools/readme.txt tools/wallpaper.jpg tools/mkfs.py .disk-sectors-flag .include-busybox-flag .disk-layout-flag
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS) $(DISK_FILES)
$(ROOT_DISK_IMG): disk.fs tools/wrap_mbr.py .disk-sectors-flag .disk-layout-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA
else
disk.fs: $(USER_BINS) $(BUSYBOX_DISK_DEPS) tools/hello.txt tools/readme.txt tools/wallpaper.jpg tools/mkext3.py .disk-sectors-flag .include-busybox-flag .disk-layout-flag
	$(PYTHON) tools/mkext3.py $@ $(FS_SECTORS) $(DISK_FILES)
$(ROOT_DISK_IMG): disk.fs tools/wrap_mbr.py .disk-sectors-flag .disk-layout-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py disk.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0x83
endif
endif

# dufs.img is the primary ATA slave (sdb), mounted at /dufs during ext3-root
# boots. It is intentionally not rebuilt by run-fresh when it already exists.
dufs.fs: tools/mkfs.py .disk-sectors-flag
	$(PYTHON) tools/mkfs.py $@ $(FS_SECTORS)
$(DUFS_IMG): dufs.fs tools/wrap_mbr.py .disk-sectors-flag | $(IMG_DIR)
	$(PYTHON) tools/wrap_mbr.py dufs.fs $@ $(PARTITION_START) $(DISK_SECTORS) 0xDA

disk.img: $(ROOT_DISK_IMG)
dufs.img: $(DUFS_IMG)

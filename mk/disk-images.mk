# Hard-disk images. disk.img is the primary root disk: ATA master on x86,
# SD media for the Pi EMMC controller on arm64 raspi3b, and a virtio-blk
# device on arm64 virt (M2.4c reuses the same MBR-wrapped image; the raw
# partition layout is identical). ROOT_FS=dufs builds sda as DUFS; the
# default builds a deterministic Linux-compatible ext3 root partition.
#
# raspi5 builds the same arm64 disk.fs (the raw ext3 image with the user
# binaries) without the wrap_mbr step, because the user assembles the SD
# card by hand: partition 1 is FAT32 (Pi firmware + kernel8.img +
# config.txt + DTBs), partition 2 receives `dd if=disk.fs of=...p2`.
# See boot-pi5/README.md.
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

# Pi 5 single-file SD card image. Two MBR partitions:
#   sda1 FAT32  Pi firmware blobs + config.txt + kernel8.img
#   sda2 ext3   the same disk.fs the virt/raspi3b builds use
# The user assembles the firmware blobs at tools/rpi5-firmware/ once (DTBs,
# start4.elf, fixup4.dat, bootcode.bin, etc. from raspberrypi/firmware).
# Then `make ARCH=arm64 PLATFORM=raspi5 [RASPI5_UART=jstsh] pi5-sd.img`
# builds kernel8.img, disk.fs, and pi5-sd.img end-to-end. Flash with
# `sudo dd if=pi5-sd.img of=/dev/rdiskN bs=1m` and the card is ready.
PI5_FW_DIR := tools/rpi5-firmware
PI5_FW_FILES := $(wildcard $(PI5_FW_DIR)/*.dtb) \
                $(wildcard $(PI5_FW_DIR)/*.elf) \
                $(wildcard $(PI5_FW_DIR)/*.dat) \
                $(wildcard $(PI5_FW_DIR)/bootcode.bin)

pi5-sd.img: FORCE kernel8.img boot-pi5/config.txt \
            tools/build-pi5-sd-image.sh tools/wrap_mbr_pi5.py \
            $(PI5_FW_FILES)
	$(MAKE) -B disk.fs
	tools/build-pi5-sd-image.sh

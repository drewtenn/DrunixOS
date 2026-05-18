#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build-pi5-sd-image.sh — assemble a single-file Pi 5 SD card image.
#
# Output layout (MBR + two partitions):
#   sectors 0..2047       MBR + reserved (1 MiB align)
#   sectors 2048..N1      FAT32 boot partition (DRUNIXBOOT, 256 MiB)
#                         populated with: Pi 5 firmware blobs from
#                         tools/rpi5-firmware/, boot-pi5/config.txt,
#                         and the freshly-built kernel8.img.
#   sectors N2..end       ext3 root partition (raw dd of disk.fs).
#
# The user then `dd if=pi5-sd.img of=/dev/diskN bs=1m` once to the SD
# card and is done. Re-runs of this script produce a deterministic
# image so a CI pipeline can ship pre-built SD images.
#
# macOS-only for now: uses newfs_msdos + hdiutil to assemble the FAT32
# partition without root. Linux support can be added with mkfs.vfat +
# mtools; not needed yet (development is on macOS).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="${OUT:-$REPO/pi5-sd.img}"
KERNEL="${KERNEL:-$REPO/kernel8.img}"
ROOTFS="${ROOTFS:-$REPO/disk.fs}"
FW_DIR="${FW_DIR:-$REPO/tools/rpi5-firmware}"
CONFIG_TXT="${CONFIG_TXT:-$REPO/boot-pi5/config.txt}"

BOOT_PART_MIB=256
ALIGN_SECTORS=2048   # 1 MiB at 512-byte sectors

case "$(uname -s)" in
    Darwin) ;;
    *)
        echo "build-pi5-sd-image.sh: only macOS is supported today" >&2
        echo "  (Linux: add an mkfs.vfat + mcopy/mtools branch here)" >&2
        exit 1
        ;;
esac

for f in "$KERNEL" "$ROOTFS" "$CONFIG_TXT"; do
    if [ ! -f "$f" ]; then
        echo "build-pi5-sd-image.sh: missing input $f" >&2
        exit 1
    fi
done
if [ ! -d "$FW_DIR" ]; then
    echo "build-pi5-sd-image.sh: missing firmware dir $FW_DIR" >&2
    exit 1
fi
if [ ! -f "$FW_DIR/bcm2712-rpi-5-b.dtb" ]; then
    echo "build-pi5-sd-image.sh: $FW_DIR/bcm2712-rpi-5-b.dtb not found" >&2
    echo "  expected Pi 5 firmware files (DTBs, start4.elf, fixup4.dat, ...)" >&2
    exit 1
fi

TMP=$(mktemp -d -t pi5-sd-XXXXXX)
MNT="$TMP/mnt"
ATTACHED_DISK=""
mkdir -p "$MNT"
cleanup() {
    set +e
    if [ -n "$ATTACHED_DISK" ]; then
        diskutil unmount "$ATTACHED_DISK" 2>/dev/null || true
        hdiutil detach -force -quiet "$ATTACHED_DISK" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

FAT_IMG="$TMP/boot.img"
echo "building FAT32 boot partition (${BOOT_PART_MIB} MiB)..."
dd if=/dev/zero of="$FAT_IMG" bs=1m count=$BOOT_PART_MIB status=none

# macOS newfs_msdos refuses plain regular files ("Cannot get partition
# offset"). Attach the raw image as a block device first, format the
# resulting /dev/diskN, then keep using that /dev/diskN for mount/copy.
ATTACHED_DISK=$(hdiutil attach -nomount -nobrowse -imagekey \
    diskimage-class=CRawDiskImage "$FAT_IMG" | head -1 | awk '{print $1}')
if [ -z "$ATTACHED_DISK" ] || [ ! -b "$ATTACHED_DISK" ]; then
    echo "build-pi5-sd-image.sh: failed to attach $FAT_IMG via hdiutil" >&2
    exit 1
fi
newfs_msdos -F 32 -v DRUNIXBOOT "$ATTACHED_DISK" >/dev/null 2>&1
diskutil mount -mountPoint "$MNT" "$ATTACHED_DISK" >/dev/null

echo "copying boot files to FAT32 partition..."
# All firmware blobs (DTBs, start*.elf, fixup*.dat, bootcode.bin). The set
# the user assembled at tools/rpi5-firmware/ stays under the user's
# control — we don't fetch from the network here.
cp "$FW_DIR"/*.dtb "$MNT/" 2>/dev/null || true
cp "$FW_DIR"/*.elf "$MNT/" 2>/dev/null || true
cp "$FW_DIR"/*.dat "$MNT/" 2>/dev/null || true
if [ -f "$FW_DIR/bootcode.bin" ]; then
    cp "$FW_DIR/bootcode.bin" "$MNT/"
fi
cp "$CONFIG_TXT" "$MNT/config.txt"
cp "$KERNEL" "$MNT/kernel8.img"

# Force a sync so hdiutil sees stable bytes before detach.
sync
diskutil unmount "$ATTACHED_DISK" >/dev/null
hdiutil detach -quiet "$ATTACHED_DISK" >/dev/null
ATTACHED_DISK=""

# Compute partition layout.
boot_size=$(stat -f %z "$FAT_IMG")
root_size=$(stat -f %z "$ROOTFS")
boot_sectors=$(( boot_size / 512 ))
root_sectors=$(( root_size / 512 ))
boot_start=$ALIGN_SECTORS
root_start=$(( (boot_start + boot_sectors + ALIGN_SECTORS - 1) /
               ALIGN_SECTORS * ALIGN_SECTORS ))
total_sectors=$(( root_start + root_sectors ))

# Build the composite image: zero the requested span, dd in each partition,
# then write the MBR table on top.
echo "composing $OUT ($((total_sectors / 2048)) MiB total)..."
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=512 count=$total_sectors status=none
dd if="$FAT_IMG" of="$OUT" bs=512 seek=$boot_start conv=notrunc status=none
dd if="$ROOTFS" of="$OUT" bs=512 seek=$root_start conv=notrunc status=none

python3 "$REPO/tools/wrap_mbr_pi5.py" "$OUT" \
    "$boot_start" "$boot_sectors" 0x0c \
    "$root_start" "$root_sectors" 0x83

echo "wrote $OUT"
echo "  partition 1 (FAT32 boot): sectors $boot_start..$((boot_start + boot_sectors - 1)), type 0x0c"
echo "  partition 2 (ext3 root):  sectors $root_start..$((root_start + root_sectors - 1)), type 0x83"
echo
echo "flash with:  sudo dd if=$OUT of=/dev/rdiskN bs=1m status=progress"
echo "         or: sudo dd if=$OUT of=/dev/mmcblk0 bs=1M status=progress  (Linux)"

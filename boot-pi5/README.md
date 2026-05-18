# Drunix on Raspberry Pi 5

This directory is a template for the FAT32 boot partition of a Pi 5 SD card.
It does *not* contain the vendor firmware blobs themselves; those are
fetched from Raspberry Pi's firmware project, not committed here, so the
repository stays source-only.

## SD card layout (M6 and beyond)

Two MBR partitions on the same microSD:

| Partition | Filesystem | Holds |
|---|---|---|
| `sda1` | FAT32 | Pi firmware (start4.elf, fixup4.dat, DTBs), `config.txt`, `kernel8.img` |
| `sda2` | ext3  | Drunix root: `bin/`, `dev/`, `proc/`, `sys/`, user binaries |

The Pi 5 firmware can only load `kernel8.img` from a FAT32 partition,
so partition 1 is reserved for it. Drunix's `arm64_mount_root_namespace`
on raspi5 mounts `/dev/sda2` as the root filesystem (see
`DRUNIX_ROOT_DEVICE` in `kernel/arch/arm64/arch.mk`).

## What goes on the SD card

### 1. Build the kernel and the root image

```
make ARCH=arm64 PLATFORM=raspi5 kernel8.img disk.fs
```

By default this targets RP1 UART0 on the 40-pin GPIO header. If you are
using the Raspberry Pi Debug Probe on the JST-SH debug header, add
`RASPI5_UART=jstsh` to the make command.

The two outputs you need:
- `kernel8.img` — flat binary loaded by the Pi firmware at 0x80000.
- `disk.fs` — raw ext3 image (~130 MiB) with the user binaries populated.

### 2. Partition the SD card

(Example numbers — substitute the actual disk identifier for your card.)

```
diskutil eraseDisk MS-DOS DRUNIXBOOT MBR /dev/diskN     # macOS
diskutil partitionDisk /dev/diskN MBR \
    MS-DOS DRUNIXBOOT 256M \
    Free DRUNIXROOT R
```

After the partition step you should have `sda1` as FAT32 (~256 MiB) and
`sda2` as raw / unformatted (the rest of the card). Drunix's ext3 image
goes onto `sda2` directly via `dd`; there's no need to format `sda2`
from the host.

### 3. Populate sda1 (FAT32 boot)

Copy these files to the FAT32 partition root:

- `config.txt` (this directory)
- `kernel8.img` (built above)
- Pi 5 firmware blobs from `raspberrypi/firmware` on GitHub:
  - `start4.elf` and `fixup4.dat` — VPU boot stage.
  - `bcm2712-rpi-5-b.dtb` — primary device tree.
  - `bcm2712d0-rpi-5-b.dtb` — D0-stepping device tree (also include
    `start.elf`, `fixup.dat`, etc. defensively).
  - `bootcode.bin` is not needed on Pi 5 (firmware lives in the SoC
    EEPROM) but is harmless if present.

  Pin to a known-good firmware release tag rather than master.

### 4. Populate sda2 (ext3 root)

```
sudo dd if=disk.fs of=/dev/diskNs2 bs=1M status=progress
```

(On Linux replace `/dev/diskNs2` with `/dev/mmcblk0p2` or wherever your
SD card's second partition lives.) The disk.fs image is already a valid
ext3 filesystem — no `mkfs.ext3` needed.

### 5. Eject, insert, boot

```
diskutil eject /dev/diskN
```

Insert the card into the Pi 5, connect your serial console (next
section), and power the board.

## Wiring the serial console

Pi 5's default debug UART when `enable_uart=1` is in `config.txt` is
RP1 UART0, wired to:

- TXD: GPIO 14 → 40-pin header pin 8
- RXD: GPIO 15 → 40-pin header pin 10
- GND: any ground pin (e.g. pin 6)

Connect a USB-to-UART adapter at 115200 8N1 and open the host-side
terminal (e.g. `screen /dev/tty.usbserial-* 115200` on macOS).

If you have the Raspberry Pi Debug Probe instead, build with
`RASPI5_UART=jstsh` and connect to the small 3-pin JST-SH debug
header on the board.

## What you should see (M6)

```
... firmware-stage Pi bootloader logs ...
NOTICE:  BL31: v2.6(release):v2.6-240-...
NOTICE:  BL31: Built : 12:55:13, Dec  4 2024
Drunix AArch64 v0 - hello from EL1
CurrentEL=0x4 (EL1)
CNTFRQ_EL0=54000000Hz
ARM64 MMU enabled
Drunix raspi5: MMU + GIC-400 + generic timer up. Mounting root and launching init.
raspi5: SDHCI disk registered as sda
ARM64 root mounted: ext3
drunix$
```

At the `drunix$` prompt you have a real `/bin/shell` (the rexy
userspace shell) running on top of the ext3 root on the SD card.

## What you should see (M7)

With the M7 `config.txt` and an HDMI monitor attached, the bring-up
sequence prints a few extra lines and the monitor lights up with
the kernel text console:

```
raspi5 fb: bringup start
raspi5 fb: mbox_base=0x000000107c013880
raspi5 fb: bus_raw=0x........
raspi5 fb: size=0x00300000
raspi5 fb: pitch=0x00001000
raspi5 fb: phys=0x00000000........
raspi5 fb: ready (fb_text_console + /dev/fb0)
Drunix raspi5: HDMI framebuffer + /dev/fb0 ready
```

If no monitor is attached, or if the firmware places the framebuffer
above the 2 GiB linear-map ceiling Drunix currently enforces, the
bring-up step prints `HDMI framebuffer unavailable (serial only)`
and boot continues to the serial shell. No reboot needed; just
attach the monitor and power-cycle.

Input on the HDMI display still requires the serial console — Pi 5
USB HID lands in M8.

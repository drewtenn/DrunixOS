# Drunix on Raspberry Pi 5

This directory is a template for the FAT32 boot partition of a Pi 5 SD card.
It does *not* contain the vendor firmware blobs themselves; those are
fetched from Raspberry Pi's firmware project, not committed here, so the
repository stays source-only.

## What goes on the SD card

1. Format the SD card with a single FAT32 boot partition.
2. Copy `config.txt` (this directory) to the partition root.
3. Build Drunix for the Pi 5:
   ```
   make ARCH=arm64 PLATFORM=raspi5 kernel8.img
   ```
   By default this targets RP1 UART0 on the 40-pin GPIO header. If you
   are using the Raspberry Pi Debug Probe on the JST-SH debug header,
   add `RASPI5_UART=jstsh` to the make command.
4. Copy `kernel-arm64.elf -> kernel8.img` (after `objcopy -O binary`)
   to the partition root.
5. Fetch the Pi 5 firmware blobs and copy them to the partition root:
   - `bootcode.bin` — not needed on Pi 5 (firmware lives in the boot
     ROM on board); harmless if present.
   - `start4.elf` and `fixup4.dat` — VPU boot stage.
   - `bcm2712-rpi-5-b.dtb` — device tree blob the firmware loads and
     hands to Drunix via `x0`.
   - Any board-revision DTBs the firmware needs (e.g.
     `bcm2712d0-rpi-5-b.dtb` on D0-stepping boards).

   These come from the `boot/` directory of `raspberrypi/firmware` on
   GitHub. Pin to a known-good firmware release tag rather than
   tracking the master branch.

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

## What you should see

```
Drunix AArch64 v0 - hello from EL1
CurrentEL=0x4 (EL1)
CNTFRQ_EL0=54000000Hz
ARM64: before MMU enable
ARM64 MMU enabled
ARM64: MMU enable returned
Drunix raspi5 MVP: MMU + GIC-400 + generic timer up. Polling UART for input.
```

After the last line the kernel is idle in `arm64_console_loop`, polling
the UART for input. Typed characters echo back through the console
terminal.

\newpage

## Chapter 33 — Standing Up Drunix on a Raspberry Pi 5

Chapters 31 and 32 covered what the Drunix AArch64 port *is* and what makes the BCM2712 different from QEMU virt and the Pi 3. This chapter covers what an operator actually does to take a freshly built kernel from the development host all the way to a `drunix:/>` prompt on the Pi 5's serial console. The flow has three distinct phases, in this order: build the SD card image on the host, flash it to a microSD, and power the Pi up with a serial cable attached. Each phase has a small number of decisions and a small number of failure modes, both of which this chapter documents.

The chapter assumes the reader already has the firmware files described in the closing paragraphs of chapter 32 sitting in `tools/rpi5-firmware/`. The build does not download these for the operator — they are vendor-controlled binaries whose versions shift with Raspberry Pi firmware releases, and committing them into the source tree would tie the OS to a particular Raspberry Pi firmware revision. The first-time setup instructions live in `boot-pi5/README.md`; this chapter starts at the point where those files are already in place.

### Build outputs

Running `make ARCH=arm64 PLATFORM=raspi5 RASPI5_UART=jstsh pi5-sd.img` from the repository root produces a complete, ready-to-flash SD card image. The build chain produces four artifacts in this order, each one feeding the next:

| Artifact | Approximate size | Role |
|---|---|---|
| `kernel-arm64.elf` | 1.5 MB | Fully linked AArch64 kernel as an ELF executable. Used for `gdb` and `addr2line` against fault PCs. Never copied to the Pi. |
| `kernel8.img` | 290 KB | Flat binary derived from the ELF via `aarch64-elf-objcopy -O binary`. Loaded by the Pi 5 EEPROM at physical address `0x80000`. |
| `disk.fs` | 127 MB | Raw ext3 filesystem image containing `/bin/shell`, `/bin/desktop`, the rest of the rexy userspace, and `tools/hello.txt` / `tools/readme.txt`. The same disk.fs the virt and Pi 3 builds use; nothing in it is Pi-5-specific. |
| `pi5-sd.img` | 384 MB | Single-file SD card image combining the FAT32 boot partition with the firmware blobs, `config.txt`, and `kernel8.img`, plus the ext3 root partition as a raw `dd` of `disk.fs`. Flashed with one `dd` invocation. |

The `RASPI5_UART=jstsh` make variable is the one decision the build asks the operator to make. It controls which physical UART the kernel uses for its debug console. Setting it to `jstsh` routes serial through `uart10` on the SoC itself, reachable via the small JST-SH "debug" connector on the Pi 5 board. Omitting the variable defaults to `RP1 UART0`, reachable on GPIO pins 8 (TXD) and 10 (RXD) of the 40-pin GPIO header. The two choices wire to different UART controllers at different physical addresses and have different physical connectors; mixing them up is the most common reason a freshly built kernel boots and produces no serial output. Chapter 32 explains the underlying address split.

### SD card layout

The `pi5-sd.img` is a single 384 MB file with an MBR partition table and two partitions. The composition is fixed by `tools/build-pi5-sd-image.sh`:

| Sectors | Partition | Filesystem | Size | Role |
|---|---|---|---|---|
| 0–2047 | (reserved) | — | 1 MB | MBR + alignment padding |
| 2048–N₁ | sda1 `DRUNIXBOOT` | FAT32 | 256 MB | Pi firmware + `config.txt` + `kernel8.img` |
| N₂–end | sda2 (unnamed) | ext3 | ~127 MB | Drunix root filesystem |

The FAT32 partition is the only one the Pi 5 firmware looks at. The Pi 5 EEPROM bootloader can read FAT32 and FAT16, but not ext3 — so the kernel image and the firmware-stage device tree have to live on FAT32. The ext3 partition is the kernel's responsibility; Drunix's SDHCI driver from chapter 32 reads it directly and mounts it as `/`.

The FAT32 partition gets thirteen files copied into it: `config.txt`, `kernel8.img`, four device-tree blobs (`bcm2712-rpi-5-b.dtb` for the standard Pi 5 plus three CM5 / 2712D0 variants the firmware will pick from based on the board it detects), three firmware ELFs (`start.elf`, `start4.elf`, `start_cd.elf`), three fixup files (`fixup.dat`, `fixup4.dat`, `fixup_cd.dat`), and `bootcode.bin` if present. The Pi 5's EEPROM no longer needs `bootcode.bin` itself, but the file is copied for compatibility with older hardware revisions.

`config.txt` is the only file an operator typically edits. The Drunix template at `boot-pi5/config.txt` contains a minimal set of directives that tell the firmware four things: target the 64-bit ARM execution state (`arm_64bit=1`), load the kernel from `kernel8.img` (`kernel=kernel8.img`), enable the firmware's own UART for early-stage logging (`enable_uart=1` plus `uart_2ndstage=1`), and request a fixed 1024×768×32 framebuffer pre-allocated for the chapter-32 HDMI mailbox driver to discover. Other Pi config.txt directives — overclocking, GPU memory split, HDMI mode overrides — are not exercised by Drunix and are left out.

### Flashing

Flashing is one `dd` invocation on the development host. On macOS the recipe is:

```
diskutil list
sudo dd if=pi5-sd.img of=/dev/rdiskN bs=1m status=progress
```

where `rdiskN` is the *raw* device node for the SD card — `rdisk4` is typical on a single-slot card reader. Identifying the right `N` before running `dd` is critical: a wrong identifier overwrites whatever else is plugged in. The `/dev/rdisk*` prefix instead of `/dev/disk*` is a macOS-specific speedup that bypasses the buffer cache; both work, but the raw node is roughly five times faster on a USB card reader. On Linux the equivalent is `sudo dd if=pi5-sd.img of=/dev/mmcblk0 bs=1M status=progress` (or the matching `/dev/sdX` if the reader presents as SCSI). After `dd` returns, `sync` and physically eject the card; macOS Finder may have automounted the FAT32 partition.

A single `pi5-sd.img` is reusable across many flashes. The image is deterministic — successive builds of the same source tree produce byte-identical images apart from the embedded build timestamp — so the file can be cached, shared, or shipped through CI without bundling private state.

### Serial console wiring

The Pi 5 has two physically distinct serial paths and Drunix can use either. The choice was already made at build time via `RASPI5_UART=jstsh` or its absence; this section describes which physical cable goes where.

The **JST-SH debug header** is a 3-pin connector near the USB-C power input. It is electrically tied to `uart10` inside the BCM2712 SoC and is the path Drunix uses when `RASPI5_UART=jstsh` was set at build time. The Raspberry Pi Debug Probe plugs directly into this header; a generic USB-UART adapter needs three jumpers (TXD, RXD, GND) cut from a JST-SH cable.

The **40-pin GPIO header** carries `RP1 UART0` on physical pins 8 (TXD), 10 (RXD), and any ground pin (6 is conventional). Drunix uses this path when the build omits `RASPI5_UART=jstsh`. Reaching RP1 UART0 from the kernel requires the PCIe2 root complex to be alive and the outbound MMIO window programmed — work that lives in `kernel/arch/arm64/platform/raspi5/pcie.c` and that the firmware initializes before kernel handoff.

Both paths run at 115200 baud, 8 data bits, no parity, 1 stop bit. The host-side terminal is typically `screen /dev/tty.usbserial-* 115200` on macOS or `picocom -b 115200 /dev/ttyUSB0` on Linux.

### The boot chain from cold power-on

When the Pi 5 is powered on with the flashed SD card inserted, the following sequence runs before any Drunix code executes:

1. The **on-chip EEPROM bootloader** runs from the BCM2712's mask ROM. It initializes the DDR controller, brings up the basic clock tree, and probes the SD card. The activity LED on the board lights solid green during this phase. The EEPROM bootloader is closed-source and version-locked to the silicon revision; an operator usually never updates it.
2. The bootloader reads `config.txt` from the FAT32 partition and parses the directives. `arm_64bit=1` selects the 64-bit boot path; `kernel=kernel8.img` names the kernel artifact; `enable_uart=1` configures the firmware UART (`uart10` on the BCM2712 SoC) for the second-stage logging that `uart_2ndstage=1` requests.
3. The bootloader loads the matching device tree blob. For a standard Pi 5 this is `bcm2712-rpi-5-b.dtb`; the EEPROM picks between the four DTBs based on the board it detected. The DTB is loaded into RAM at an address the bootloader picks, and a pointer to it is held in register `x0`.
4. The bootloader loads `kernel8.img` to physical address `0x80000` — a hard-coded address required by the Pi boot protocol. All four Cortex-A76 cores are released to the kernel entry point at EL2 with caches and MMU off; only CPU 0 will execute past the early park spin in `boot.S`. The FDT pointer is in `x0`; all other registers are unspecified.
5. The firmware also runs the ARM Trusted Firmware build (`bl31.bin` embedded in the EEPROM image) that establishes EL3 reset vectors and a small set of SMC handlers, then drops to EL2 for the kernel handoff. The Drunix kernel never invokes ARM Trusted Firmware services in M5 through M8 and does not need to know it is present.

At step 5 Drunix takes over. Chapter 31 covers the EL2-to-EL1 demotion in `boot.S` and chapter 32 covers the platform-specific MMU and GIC bring-up. From the operator's perspective, the visible artefact of a successful boot is the serial trace that begins with the first kernel print over `uart10` (or RP1 UART0):

```
Drunix AArch64 v0 - hello from EL1
CurrentEL=0x4 (EL1)
CNTFRQ_EL0=54000000Hz
ARM64 MMU enabled
raspi5 fb: bringup start
...
Drunix raspi5: HDMI framebuffer + /dev/fb0 ready
Drunix raspi5: MMU + GIC-400 + generic timer up. Mounting root and launching init.
raspi5: SDHCI disk registered as sda
ARM64 root mounted: ext3
drunix shell -- type 'help' for commands
drunix:/>
```

The `drunix:/>` prompt is the success condition. Between the `raspi5 fb` and `drunix shell` lines, an operator with an HDMI monitor attached will also see the kernel boot log mirrored on the monitor via the chapter-32 HDMI framebuffer.

### Diagnosing a silent boot

The most common Pi 5 first-boot failure mode is "no output at all on the host serial terminal." There are three failure modes and each has a one-step diagnostic:

| Symptom | Likely cause | Check |
|---|---|---|
| Activity LED never lights | EEPROM did not see the SD card | reseat the card; try another known-good card |
| Activity LED solid green, no serial | Wrong physical UART selected at build time | rebuild with the other `RASPI5_UART` setting and reflash |
| Activity LED solid green, garbled serial | Wrong baud rate on the host terminal | confirm 115200 8N1 |

The "solid green LED, no serial" case is the most operationally annoying because the green LED gives no signal that the firmware finished, jumped to Drunix, and started writing to a UART the host is not listening on. The build defaults to `RP1 UART0` (the 40-pin GPIO header path), so an operator who plugs into the JST-SH connector without also passing `RASPI5_UART=jstsh` at build time gets exactly this silence.

A working sanity check that bypasses Drunix entirely is to set `uart_2ndstage=1` (already in the template `config.txt`) and watch for firmware-stage messages — typically a banner naming the EEPROM version — before any kernel output appears. If the firmware lines never show up on the serial terminal, the UART pairing is wrong and the kernel never got a chance.

### Where the Machine Is by the End of Chapter 33

A reader who has worked through this chapter starts with a freshly-built `pi5-sd.img` on the development host and ends with a `drunix:/>` shell prompt over serial on a Pi 5. The Pi 5 is in the same in-kernel state chapter 32 ends in: MMU is on, the GIC-400 is forwarding the generic-timer PPI, the HDMI framebuffer (if a monitor is attached) carries a mirror of the kernel boot console, the SDHCI driver has mounted the ext3 root from the SD card's second partition, and `/bin/shell` is running with stdin and stdout wired to the serial UART. The follow-on M8 milestone — covered in `docs/design/m8-raspi5-usb-keyboard.md` rather than a chapter, because it is still in flight — replaces serial input with a USB keyboard reached through the BCM2712 PCIe link to RP1, but the pre-shell boot sequence stays exactly as documented here.

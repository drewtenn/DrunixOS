\newpage

## Chapter 31 — AArch64 Platform Notes

This appendix is a peripheral-reference companion for AArch64 readers. It does not re-narrate the bring-up flow: that material lives in ch01 (boot handoff and EL2→EL1 demotion), ch03 (BCM2835 mini-UART as the first console), ch04 (exception vector table at `VBAR_EL1`), and ch05 (ARM Generic Timer plus BCM2836 core-local interrupt routing). What this chapter collects is the platform-specific address and register detail for the Raspberry Pi 3 (BCM2837 SoC) and its QEMU `raspi3b` model — the same role that ch10 and ch11 play for the PC keyboard and ATA disk peripherals, and that ch27 plays for the PS/2 mouse.

### BCM2835 Mini-UART

The BCM2835 auxiliary block — which contains the mini-UART and two SPI controllers — is memory-mapped at physical base address **`0x3F215000`** on the Raspberry Pi 3. The bring-up sequence in ch03 writes to registers at the following offsets from that base:

| Offset | Register | Purpose |
|--------|----------|---------|
| `0x04` | `AUX_ENB` | Enable bit for auxiliary peripherals; bit 0 enables the mini-UART |
| `0x60` | `AUX_MU_IO` | Data register: write a byte here to transmit, read to receive |
| `0x64` | `AUX_MU_IER` | Interrupt enable; zeroed during bring-up |
| `0x68` | `AUX_MU_IIR` | Interrupt identify and FIFO control; FIFO reset here |
| `0x6C` | `AUX_MU_LCR` | Line control; bit 1 set selects 8-bit mode |
| `0x70` | `AUX_MU_MCR` | Modem control; zero during bring-up |
| `0x74` | `AUX_MU_LSR` | Line status; bit 5 is the transmitter-empty flag |
| `0x7C` | `AUX_MU_CNTL` | Control register; bits 0–1 enable TX and RX |
| `0x80` | `AUX_MU_BAUD` | Baud rate divisor |

The baud divisor formula is `(system_clock_hz / (8 × baud_rate)) − 1`. On the `raspi3b` QEMU model the system clock runs at 250 MHz, giving a divisor of `270` for 115200 baud. On real Pi 3 hardware the system clock is typically 400 MHz, giving `433`.

GPIO14 and GPIO15 must be switched to **alternate function 5** (the BCM2835 routing for the mini-UART TX and RX lines) before the mini-UART can communicate with the outside world. Function selection is written through the GPIO function-select registers in the main peripheral block at `0x3F200000`.

### BCM2836 Core-Local Interrupt Block

The BCM2836 core-local interrupt block is a Pi 3-specific peripheral at physical base address **`0x40000000`**. Each of the four CPU cores has a 4-byte register at `0x40000040 + (4 × core_number)` that controls which interrupt sources that core will receive. Writing `0x02` to core 0's register (`0x40000040`) enables the non-secure EL1 physical timer interrupt on core 0 — the step that connects the ARM Generic Timer's countdown expiry to core 0's IRQ slot in the exception vector table.

The same block exposes a per-core interrupt source register at `0x40000060 + (4 × core_number)`. The IRQ handler reads core 0's source register (`0x40000060`) on every interrupt to confirm that the pending source is the timer (bit 1) before reloading `CNTP_TVAL_EL0` and returning.

### VideoCore Mailbox Property Interface

The VideoCore GPU embedded in the BCM2837 controls the HDMI display and the framebuffer. The kernel communicates with the GPU through the **VideoCore mailbox** at physical address **`0x3F00B880`**. The mailbox provides a property channel (channel 8) through which the kernel sends a tagged property message asking the GPU to allocate and configure a framebuffer — base address, dimensions, bits-per-pixel, and pitch — and the GPU responds with the physical address and size of the allocated buffer.

The framebuffer bring-up sequence using this interface is planned as part of milestone 6. The mailbox address and channel protocol are documented here as a reference point for that future work.

### Pi 3 Fixed Memory Layout

The Raspberry Pi 3 (BCM2837 SoC) presents a fixed address map in the `raspi3b` QEMU model. Chapter 7 derives the usable RAM range from this layout; it is collected here for quick reference.

| Region | Start | End | Notes |
|--------|-------|-----|-------|
| Usable RAM | `0x00000000` | `0x3EFFFFFF` | 1 GB minus the device window |
| Device window | `0x3F000000` | `0x40000000` | Peripheral bus and VideoCore mailbox |
| Core-local block | `0x40000000` | `0x40000100` | BCM2836 per-core interrupt registers |
| Kernel image | `0x00080000` | — | Load address placed by QEMU `raspi3b` loader |

The device window boundary at `0x3F000000` is the reason ch07 records that exact address as the top of usable RAM on AArch64. Any physical allocator that treats the device window as free pages would corrupt hardware-mapped registers on the first access.

### Boot Entry Contract

The QEMU `raspi3b` loader places the kernel flat binary at physical address `0x80000` and transfers control to that address on all four cores simultaneously. Chapter 1 covers the full boot handoff, including how the stub selects core 0 as primary and parks the others. The short list below summarises the machine state at the first instruction:

- All four cores are in AArch64 state at **EL2** (the hypervisor privilege level).
- The program counter holds `0x80000`.
- No Multiboot structure, no device tree pointer in any register — boot parameters come from the fixed hardware layout.
- Caches and the MMU are off.
- No stack pointer has been established.

The bring-up stub in ch01 reads `MPIDR_EL1` to identify core 0, parks cores 1–3, programs `HCR_EL2`, `SCTLR_EL1`, and `ELR_EL2`, and issues `eret` to drop to EL1 before establishing a stack and zeroing `.bss`.

### Where the Machine Is

This appendix is background reference: the physical addresses, register maps, and entry-state facts that anchor the narrative in ch01/ch03/ch04/ch05. No new machine state is established here — by the time a reader reaches ch31, all of those paths are already live.

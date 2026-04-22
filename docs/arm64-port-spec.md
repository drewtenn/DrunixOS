# AArch64 / Raspberry Pi Port — Milestone 1 Specification

Companion to `docs/arm64-port-plan.md`.  This document specifies the
concrete implementation of **Milestone 1** only — "Hello, ARM" in
QEMU.  Scope, file layout, registers, and verification criteria are
all enumerated so that the implementation step is a straight transcription.

Nothing in this commit touches `kernel/`, `boot/`, `user/`, `iso/`,
or the root `Makefile`.  This is a specification only.

## Exit criterion

```
$ make ARCH=arm64 run
Drunix AArch64 v0 - hello from EL1
CurrentEL=0x4 (EL1)
CNTFRQ_EL0=62500000Hz
tick 1
tick 2
tick 3
tick 4
tick 5
```

Five ticks must arrive within 10 seconds.  A headless `make ARCH=arm64
check` target greps `logs/serial-arm.log` for `tick 5`.

The existing `make` (no `ARCH`) and `make check` must pass unchanged.

## Target hardware

- **Board:** Raspberry Pi 3 Model B / B+ (BCM2837, Cortex-A53, 4 cores).
- **Emulator:** `qemu-system-aarch64 -M raspi3b`.
- **Peripheral base:** `0x3F000000` (Pi 3 legacy bus mapping).
- **Local peripheral base:** `0x40000000` (BCM2836 core-local block).
- **Kernel load address:** `0x80000` physical.
- **Initial exception level:** EL2 (all four cores, per Pi firmware
  and QEMU convention).

## File layout

All new files live under `kernel/arch/arm64/`.  No other directories
are added.

```
kernel/arch/arm64/
├── arch.mk           # Make fragment: toolchain, CFLAGS, QEMU command
├── linker.ld         # ENTRY(_start), . = 0x80000
├── boot.S            # _start: EL2→EL1 drop, core park, bss zero, SP setup
├── exceptions.S      # 16-entry VBAR_EL1 table, saves regs, dispatches to C
├── exceptions.c      # Sync/IRQ/SError C handlers
├── irq.c             # BCM2836 core-local IRQ routing
├── timer.c           # ARM Generic Timer (CNTP) init and IRQ handler
├── timer.h
├── uart.c            # BCM2835 mini-UART driver (blocking putc/puts/getc)
├── uart.h
└── start_kernel.c    # arm64_start_kernel() — banner, timer, heartbeat loop
```

### `arch.mk`

Per-architecture Make fragment that the root `Makefile` `-include`s
after resolving `ARCH`.  Provides:

```
ARM_CC       := aarch64-elf-gcc
ARM_LD       := aarch64-elf-ld
ARM_OBJCOPY  := aarch64-elf-objcopy

ARM_CFLAGS   := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                -mcpu=cortex-a53 -mgeneral-regs-only \
                -nostdlib -Wall -Wextra -g -O2
ARM_LDFLAGS  := -nostdlib -T kernel/arch/arm64/linker.ld

ARM_KOBJS    := kernel/arch/arm64/boot.o \
                kernel/arch/arm64/exceptions_s.o \
                kernel/arch/arm64/exceptions.o \
                kernel/arch/arm64/irq.o \
                kernel/arch/arm64/timer.o \
                kernel/arch/arm64/uart.o \
                kernel/arch/arm64/start_kernel.o \
                kernel/lib/kprintf.o kernel/lib/kstring.o

QEMU_ARM     := qemu-system-aarch64
QEMU_ARM_MACHINE := raspi3b
```

The ARM build links only `ARM_KOBJS` — nothing from `KOBJS` — so
that nothing x86-specific is dragged in.  `kernel/lib/kprintf.c` and
`kernel/lib/kstring.c` are pure C with no arch assumptions and are
reused.  `kernel/lib/klog.c` is **not** reused in Milestone 1; it
depends on `clock.h`, `sched.h`, VGA `print_string`, and the QEMU
debugcon port.  klog is ported in Milestone 2.

### `linker.ld`

```
OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    . = 0x80000;
    _kernel_start = .;
    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) }
    . = ALIGN(0x1000);
    .data   : { *(.data*)   }
    . = ALIGN(16);
    __bss_start = .;
    .bss    : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    _kernel_end = .;
}
```

`.text.boot` first guarantees `_start` is at `0x80000`.

### `boot.S` — entry and EL2→EL1 drop

```
.section ".text.boot"
.globl _start
_start:
    // Park cores 1..3.
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbnz    x1, .Lpark

    // Check current exception level.
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    .Lfrom_el2
    cmp     x0, #1
    b.eq    .Lin_el1
    b       .Lpark              // EL3 / unexpected

.Lfrom_el2:
    // EL1 will run AArch64.
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    // SCTLR_EL1: RES1 bits set, MMU off, caches off.
    mov     x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0

    // Return to EL1h with DAIF masked.
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, .Lin_el1
    msr     elr_el2, x0
    eret

.Lin_el1:
    // Stack grows down from _start (0x80000).
    adr     x0, _start
    mov     sp, x0

    // Zero .bss.
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    bl      arm64_start_kernel
    // Fall through to park if C returns.

.Lpark:
    wfe
    b       .Lpark
```

### `exceptions.S` — vector table

AArch64 requires the vector base to be 2 KB (0x800) aligned.  Each
of the 16 entries is 0x80 bytes.  Every stub saves x0–x30, passes
the trap class to a C dispatcher, then restores and `eret`s.

```
.macro VENTRY label
.balign 0x80
    b       \label
.endm

.balign 0x800
.globl vectors_el1
vectors_el1:
    VENTRY sync_sp0    // Current EL with SP_EL0
    VENTRY irq_sp0
    VENTRY fiq_sp0
    VENTRY error_sp0

    VENTRY sync_spx    // Current EL with SP_ELx (normal kernel)
    VENTRY irq_spx
    VENTRY fiq_spx
    VENTRY error_spx

    VENTRY sync_a64    // Lower EL, AArch64 (unused in Milestone 1)
    VENTRY irq_a64
    VENTRY fiq_a64
    VENTRY error_a64

    VENTRY sync_a32    // Lower EL, AArch32 (unused on this target)
    VENTRY irq_a32
    VENTRY fiq_a32
    VENTRY error_a32
```

Each stub shape:
```
sync_spx:
    SAVE_GPRS
    mov     x0, sp
    bl      arm64_sync_handler
    RESTORE_GPRS
    eret
irq_spx:
    SAVE_GPRS
    mov     x0, sp
    bl      arm64_irq_handler
    RESTORE_GPRS
    eret
// ... and similar for fiq_spx, error_spx, and the SP0 / lower-EL slots
```

`SAVE_GPRS` / `RESTORE_GPRS` are `stp`/`ldp` pairs over x0–x30.

### `exceptions.c` — C handlers

```c
void arm64_sync_handler(void);
void arm64_irq_handler(void);
void arm64_fiq_handler(void);
void arm64_serror_handler(void);
```

`arm64_sync_handler()` reads `ESR_EL1`, `ELR_EL1`, `FAR_EL1`, prints
them via `uart_puts`, and halts (`wfe` loop).  No recovery in
Milestone 1 — we do not expect synchronous exceptions on this
happy path.

`arm64_irq_handler()` reads the BCM2836 core-local IRQ source
register at `0x40000060` (core 0), identifies the CNTPNS timer
bit, and calls `arm64_timer_irq()`.  Unknown IRQs increment a spurious
counter.

### `irq.c` — BCM2836 core-local routing

```c
#define CORE0_TIMER_IRQCNTL   (*(volatile uint32_t *)0x40000040)
#define CORE0_IRQ_SOURCE      (*(volatile uint32_t *)0x40000060)
#define CNTPNSIRQ_BIT         (1u << 1)

void arm64_irq_enable_timer(void)
{
    CORE0_TIMER_IRQCNTL = CNTPNSIRQ_BIT;   // route EL1 non-secure
                                           // physical timer to IRQ
}
```

### `timer.c` — ARM Generic Timer

```c
static inline uint64_t cntfrq(void)
{
    uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}
static inline void cntp_tval_write(uint64_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}
static inline void cntp_ctl_write(uint64_t v)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

static uint64_t ticks_per_interval;
static volatile uint64_t tick_count;

void arm64_timer_init(uint32_t hz)
{
    ticks_per_interval = cntfrq() / hz;
    cntp_tval_write(ticks_per_interval);
    cntp_ctl_write(1);             // ENABLE=1, IMASK=0
    arm64_irq_enable_timer();
}

void arm64_timer_irq(void)
{
    cntp_tval_write(ticks_per_interval);
    tick_count++;
}

uint64_t arm64_timer_ticks(void) { return tick_count; }
```

### `uart.c` — BCM2835 mini-UART

```c
#define MMIO_BASE        0x3F000000UL
#define GPFSEL1          (*(volatile uint32_t *)(MMIO_BASE + 0x200004))
#define GPPUD            (*(volatile uint32_t *)(MMIO_BASE + 0x200094))
#define GPPUDCLK0        (*(volatile uint32_t *)(MMIO_BASE + 0x200098))
#define AUX_ENABLE       (*(volatile uint32_t *)(MMIO_BASE + 0x215004))
#define AUX_MU_IO        (*(volatile uint32_t *)(MMIO_BASE + 0x215040))
#define AUX_MU_IER       (*(volatile uint32_t *)(MMIO_BASE + 0x215044))
#define AUX_MU_IIR       (*(volatile uint32_t *)(MMIO_BASE + 0x215048))
#define AUX_MU_LCR       (*(volatile uint32_t *)(MMIO_BASE + 0x21504C))
#define AUX_MU_MCR       (*(volatile uint32_t *)(MMIO_BASE + 0x215050))
#define AUX_MU_LSR       (*(volatile uint32_t *)(MMIO_BASE + 0x215054))
#define AUX_MU_CNTL      (*(volatile uint32_t *)(MMIO_BASE + 0x215060))
#define AUX_MU_BAUD      (*(volatile uint32_t *)(MMIO_BASE + 0x215068))
```

`uart_init()`:
1. `AUX_ENABLE |= 1` — enable mini-UART block.
2. `AUX_MU_CNTL = 0` — disable TX/RX during config.
3. `AUX_MU_IER = 0` — mask interrupts.
4. `AUX_MU_LCR = 3` — 8-bit mode (bit 1 must be set on the Pi
   mini-UART; the BCM2835 datasheet erratum).
5. `AUX_MU_MCR = 0` — RTS high.
6. `AUX_MU_BAUD = 270` — ≈ 115200 on a 250 MHz core clock.
7. GPIO14/15 alt5 via `GPFSEL1`; pull-up/down sequence via `GPPUD`
   + `GPPUDCLK0` (ignored by QEMU, required on real HW).
8. `AUX_MU_CNTL = 3` — enable TX+RX.

`uart_putc(c)` spins on `AUX_MU_LSR & 0x20` (TX empty), then writes
`AUX_MU_IO = c`.  `uart_puts` loops over the string and emits `\r`
before every `\n`.

### `start_kernel.c`

```c
extern char vectors_el1[];

void arm64_start_kernel(void)
{
    uart_init();
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vectors_el1));

    uart_puts("Drunix AArch64 v0 - hello from EL1\n");
    /* Print CurrentEL, CNTFRQ_EL0 using k_snprintf from kernel/lib. */

    arm64_timer_init(10);                 // 10 Hz
    __asm__ volatile("msr daifclr, #2");  // unmask IRQs

    uint64_t last = 0;
    for (;;) {
        __asm__ volatile("wfi");
        uint64_t now = arm64_timer_ticks();
        while (last < now) {
            last++;
            /* uart_puts("tick %u\n", last); using k_snprintf */
        }
    }
}
```

## Root `Makefile` changes

At the top:

```
ARCH ?= x86
-include kernel/arch/$(ARCH)/arch.mk
```

All x86-only blocks (CC, NASM, `-m32`, `-m elf_i386`, `qemu-system-i386`,
GRUB ISO pipeline, disk images, user programs, ISO, ext3 tests, doc
build) are guarded by `ifeq ($(ARCH),x86)`.

For `ARCH=arm64`:

```
kernel-arm64.elf: $(ARM_KOBJS)
	$(ARM_LD) $(ARM_LDFLAGS) -o $@ $(ARM_KOBJS)

kernel8.img: kernel-arm64.elf
	$(ARM_OBJCOPY) -O binary $< $@

run: kernel-arm64.elf
	$(QEMU_ARM) -M $(QEMU_ARM_MACHINE) -kernel kernel-arm64.elf \
		-serial null -serial stdio -nographic -no-reboot
```

`raspi3b` routes the mini-UART (`UART1`) to the **second** `-serial`
stanza, hence the `null`+`stdio` pair.  PL011 (not used here) is the
first `-serial`.

Pattern rules for the new AArch64 sources:

```
kernel/arch/arm64/%.o: kernel/arch/arm64/%.c
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

kernel/arch/arm64/%.o: kernel/arch/arm64/%.S
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@
```

`kernel/lib/kprintf.o` and `kernel/lib/kstring.o` compile with
`ARM_CFLAGS` when `ARCH=arm64` (override via a conditional `%.o: %.c`
rule guarded by `ifeq ($(ARCH),arm64)`).

## Host tool additions

Added to `README.md` dependency list as "Optional (AArch64 target)":

- `aarch64-elf-gcc`, `aarch64-elf-binutils` (or the `aarch64-linux-gnu`
  equivalents; `arch.mk` can be overridden via `make ARM_CC=... ARM_LD=...`).
- `qemu-system-aarch64`.

## Explicitly out of scope for Milestone 1

- **MMU, caches.**  Kernel runs with the MMU disabled and D-cache off.
  This is acceptable for < 1 MB of code exercising UART and a 10 Hz
  timer.
- **SMP.**  Cores 1–3 park in `_start`.
- **Syscalls, processes, ELF, userland.**  None of `kernel/proc/`,
  `kernel/mm/`, `kernel/fs/`, `kernel/drivers/`, `kernel/gui/` is
  compiled into the AArch64 image.
- **klog.**  See Milestones 2 risk note.  Direct `uart_puts` is used.
- **Real Pi 3 hardware.**  Milestone 1b handles SD image layout and
  firmware blobs.
- **Renaming `kernel/arch/` → `kernel/arch/x86/`.**  Deferred to
  Milestone 2 where it actually reduces code.  For Milestone 1 the
  x86 objects keep their existing paths and the ARM tree lives
  alongside as `kernel/arch/arm64/`.

## Verification

1. `make ARCH=arm64 run` shows the exit-criterion transcript above.
2. A new `make ARCH=arm64 check` target boots QEMU headless
   (`-display none -serial null -serial file:logs/serial-arm.log`),
   waits up to 10 s for the substring `tick 5` in the log, and
   exits non-zero otherwise.  Modeled on the existing
   `qemu_headless_until_log` macro (`Makefile` lines 192–194).
3. `make` with no `ARCH` and `make check` continue to pass unchanged.
4. Exception path is exercised by a dev-only `svc #0` from the
   banner code; confirm `arm64_sync_handler` logs `ESR_EL1` class
   0x15.  (Not required for Milestone 1 pass.)
5. Documentation: a new `docs/ch31-aarch64-bringup.md` is added
   alongside the implementation, added to `DOCS_SRC`, and built
   with `make docs`.

## Deliverables summary

| Path | New / Modified | Purpose |
|---|---|---|
| `kernel/arch/arm64/arch.mk` | new | Toolchain, flags, QEMU |
| `kernel/arch/arm64/linker.ld` | new | ELF layout, 0x80000 base |
| `kernel/arch/arm64/boot.S` | new | `_start`, EL2→EL1, park, bss, SP |
| `kernel/arch/arm64/exceptions.S` | new | 16-entry vector table |
| `kernel/arch/arm64/exceptions.c` | new | Sync/IRQ/SError C handlers |
| `kernel/arch/arm64/irq.c` | new | BCM2836 core-local routing |
| `kernel/arch/arm64/timer.c`, `.h` | new | Generic Timer EL1 physical |
| `kernel/arch/arm64/uart.c`, `.h` | new | Mini-UART driver |
| `kernel/arch/arm64/start_kernel.c` | new | Banner + heartbeat loop |
| `Makefile` | modified | `ARCH ?= x86`, `-include` arch fragment |
| `README.md` | modified | AArch64 build section |
| `docs/ch31-aarch64-bringup.md` | new | Book chapter for Milestone 1 |
| `docs/sources.mk` | modified | Include new chapter |

Everything else in the tree is unchanged.

\newpage

## Chapter 31 — AArch64 Bring-Up

### A Second Boot Path

The first thirty chapters built a kernel around one machine model: 32-bit x86, GRUB, Multiboot1, the 8259 PIC, the PIT, and `int 0x80`. That path still matters, and it remains the mainline Drunix boot for everything we have built so far. But a kernel that cannot survive outside one narrow boot environment has not really separated its operating-system logic from its machine-specific glue.

This chapter starts that separation with a deliberately tiny target: boot the kernel as a 64-bit AArch64 image under `qemu-system-aarch64 -M raspi3b`, print a banner over the Raspberry Pi mini-UART, take timer interrupts from the ARM Generic Timer, and count five heartbeat ticks. Nothing more. No paging, no user processes, no filesystems, and no desktop. The goal is not to "port Drunix" in one jump. The goal is to prove that Drunix can stand up on a second architecture without dragging the whole x86 machine model along with it.

### Why The First Slice Is So Small

Porting an operating system tempts us to chase the most visible end state: a shell prompt, a mounted filesystem, maybe even the desktop. That is exactly how a port turns into a pile of half-debugged subsystems. The safe move is to ask a smaller question first: can the new CPU start executing our code, reach a known exception level, talk to one output device, and survive periodic interrupts?

Those pieces form the narrow waist of the port:

- the boot entry point
- exception-vector installation
- one console device
- one timer interrupt source

If any of those are wrong, every later subsystem is harder to inspect. If they are right, the rest of the port has a dependable serial trace and a known-good interrupt heartbeat to build on.

### Booting Without GRUB

The x86 path enters through GRUB with a Multiboot info structure in hand. The Raspberry Pi bring-up path does not get that luxury. QEMU's `raspi3b` machine loads the kernel image at physical address `0x80000` and starts all four cores in AArch64 state, usually at **EL2** (Exception Level 2, a privileged mode intended for hypervisors). That means the bring-up code has to perform a few chores the x86 kernel inherited from firmware:

- identify the primary core and park the others
- inspect the current exception level
- drop from EL2 to **EL1** (Exception Level 1, the standard privileged mode for ordinary operating system kernels)
- establish a stack pointer
- zero `.bss`

The EL2 to EL1 transition matters because the rest of the kernel is written as an ordinary EL1 kernel, not as a hypervisor. The boot stub programs `HCR_EL2` so EL1 executes AArch64 instructions, seeds `SCTLR_EL1` with the architecturally required reset bits, points `ELR_EL2` at the EL1 continuation label, and returns with `eret`.

### Exception Vectors Replace The IDT

On x86, Chapter 4 introduced the IDT: a table of gate descriptors that the CPU consults when an interrupt or fault arrives. AArch64 uses a different shape but solves the same problem. The kernel writes the base address of a 2 KB vector table to `VBAR_EL1`. That table contains sixteen fixed slots covering:

- synchronous exceptions
- IRQs
- FIQs
- system errors

for four contexts:

- current EL using `SP_EL0`
- current EL using `SP_ELx`
- lower EL in AArch64 state
- lower EL in AArch32 state

Milestone 1 keeps the handlers intentionally blunt. The synchronous path prints register state and halts. The IRQ path recognises only one source, the non-secure EL1 physical timer interrupt, and dispatches it to the timer code. That is enough for a heartbeat kernel and no more.

### The Mini-UART As The First Console

The x86 kernel grew up around VGA text mode, framebuffer output, and the QEMU debug console. None of those exists as a natural first step on the Raspberry Pi. The simplest always-on output path in the `raspi3b` model is the BCM2835 auxiliary mini-UART.

Bringing it up is mostly register programming:

1. enable the auxiliary block
2. disable TX and RX during configuration
3. select 8-bit mode
4. set the baud divisor
5. switch GPIO14 and GPIO15 to alternate function 5
6. enable TX and RX again

Once that is done, serial output becomes a plain busy-wait loop on the transmitter-empty bit. That is exactly what early bring-up needs: one reliable way to print progress without depending on interrupts, memory management, or a framebuffer.

### The First Interrupt Source

The x86 kernel's periodic heartbeat came from the PIT routed through the PIC. On the Raspberry Pi 3 bring-up path, that role is split between two ARM-specific pieces:

- the ARM Generic Timer, which counts at `CNTFRQ_EL0`
- the BCM2836 core-local interrupt block, which routes the timer interrupt to core 0

The timer setup is refreshingly small. The kernel reads `CNTFRQ_EL0`, divides it by the desired frequency, writes the interval to `CNTP_TVAL_EL0`, enables the timer via `CNTP_CTL_EL0`, and asks the core-local interrupt controller to deliver the non-secure EL1 physical timer interrupt as an IRQ. Every IRQ rewrites the timer interval and increments a tick counter.

We pick one interrupt per second: slow enough to see distinct ticks on the console, fast enough to prove the interrupt path works reliably.

That tick counter drives the visible proof of life:

```text
Drunix AArch64 v0 - hello from EL1
CurrentEL=0x4 (EL1)
CNTFRQ_EL0=62500000Hz
tick 1
tick 2
tick 3
tick 4
tick 5
```

### What This Milestone Deliberately Does Not Solve

This bring-up path runs with the MMU disabled and caches off. It does not attempt to reuse the physical allocator, paging code, ELF loader, scheduler, or VFS. That restraint is the point. Those subsystems all depend on architecture contracts that still belong to x86 in the current tree. Milestone 1 is valuable precisely because it avoids pretending those contracts are already portable.

The result is small but honest: Drunix now has a second boot path that proves the CPU, exception, UART, and timer foundations can exist outside the original x86 environment. The next milestones can build a proper architecture boundary on top of that proof instead of trying to invent one in the dark.

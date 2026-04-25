\newpage

## Chapter 6 — Floating-Point and Vector Enablement

### FP/SIMD Ships Disabled

Chapter 5 left the IRQ dispatch registry in place and the wall clock ticking. Before we create any processes, there is a piece of hardware setup that must happen first on every architecture we support: explicitly enabling the floating-point and vector instruction sets that the CPU keeps locked off by default.

Modern CPUs ship from the factory with their floating-point and **SIMD** (Single Instruction, Multiple Data — the class of instruction that operates on several values at once, such as "add four 32-bit floats simultaneously") units disabled at boot. The reason is the same everywhere: these units carry their own register files, and saving and restoring an extra set of registers across every context switch costs time. By keeping the units off at reset, the hardware forces the operating system to make a deliberate choice: pay that per-task save/restore cost only when the kernel is ready to manage it. Until the kernel opts in, any FP or SIMD instruction raises a fault.

Enabling FP/SIMD is a small, arch-specific control-register dance. The details differ between x86 and AArch64, but the result is the same: the CPU accepts FP/SIMD instructions, the kernel captures a clean initial FP state to hand to every new process, and the scheduler is wired to save and restore that state on each context switch.

### On x86: Enabling SSE2

The particular extension we enable on x86 is **SSE2** (Streaming SIMD Extensions 2, a family of 128-bit vector instructions introduced by Intel in 2000). SSE2 provides eight 128-bit registers named `XMM0` through `XMM7`, each wide enough to hold four 32-bit floats, two 64-bit doubles, or various combinations of integer types. Modern compilers can use SSE2 aggressively even for code that looks like ordinary C, but we deliberately disable compiler-generated SSE for kernel C so that only explicit low-level FPU/SSE ownership code touches those registers.

If we do not enable SSE2 before the first explicit `fxsave` or `fxrstor` executes, the CPU raises an **Invalid Opcode exception** (`#UD`, short for "undefined instruction"). Our handler from Chapter 4 would catch the fault, print an error, and halt. This is exactly what happens if you try to manage per-process FPU state without the setup described here.

#### The Four Control Bits That Gate SSE

Four bits across two **control registers** (`CR0` and `CR4`) determine whether SSE instructions are legal:

| Register | Bit | Name | After reset | Meaning when set |
|----------|-----|------|-------------|------------------|
| `CR0` | 1 | MP (Monitor Coprocessor) | 0 | Tells the CPU to check the task-switched flag before FPU use |
| `CR0` | 2 | EM (Emulate) | 1 | Every FPU or SSE instruction raises `#UD` — as if there were no FPU at all |
| `CR4` | 9 | OSFXSR | 0 | The OS promises to use `FXSAVE` and `FXRSTOR` to save SSE state on context switches |
| `CR4` | 10 | OSXMMEXCPT | 0 | Routes SSE floating-point exceptions to a specific vector instead of `#UD` |

When GRUB hands control over, `EM` is set and `OSFXSR` is clear. Until both change, every SSE instruction is illegal. We therefore have to:

1. Clear `EM` to stop emulating the FPU.
2. Set `MP` so the CPU properly tracks the task-switched flag.
3. Set `OSFXSR` to tell the CPU we know how to save and restore SSE state.
4. Set `OSXMMEXCPT` so that SSE exceptions become their own vector rather than being reported as invalid opcodes.

All four changes are made in a tiny assembly startup routine, because reading and writing control registers cannot be expressed in C. This is the very first floating-point-related work the kernel does, so it can capture a clean FPU template before any process is launched.

#### Capturing a Clean FPU Snapshot

Once the control bits are in place, we initialise the floating-point unit with the `fninit` instruction (which resets the x87 FPU — the legacy floating-point unit present since the 80387 — to a well-defined state: all exceptions masked, double-extended precision, stack pointer zeroed) and then use `fxsave` (FP state eXtended save) to dump the complete FPU and SSE register state into a 512-byte buffer. That buffer becomes the **template** every new process is initialised from.

The template contains these well-known values:

- `FCW = 0x037F` — the FPU control word, with all x87 floating-point exceptions masked.
- `FSW = 0` — the status word, with no exceptions pending.
- `MXCSR = 0x1F80` — the SSE control register, with all SSE exceptions masked and rounding set to nearest.
- All eight `XMM` registers zeroed.

Any process created from this template starts life with a clean, predictable floating-point environment.

#### Giving Every Process Its Own FPU State

A process's in-memory descriptor needs a place to store its own 512 bytes of SSE state, because different processes will have different floating-point values in their `XMM` registers at any given moment. The `process_t` struct is extended with a 512-byte `fpu_state` field:

```c
typedef struct __attribute__((aligned(16))) {
    uint32_t pd_phys;
    uint32_t entry;
    uint32_t user_stack;
    uint32_t kstack_top;
    uint8_t  fpu_state[512];   /* FXSAVE image */
} process_t;
```

The `FXSAVE` instruction requires the destination address to be **16-byte aligned**, or the CPU raises a general protection fault. The alignment requirement comes from the hardware: the CPU uses aligned 128-bit loads and stores to move the XMM registers in and out of memory, and those instructions fault on misaligned addresses. The struct is marked as aligned to sixteen bytes, and the four preceding `uint32_t` fields total sixteen bytes, so `fpu_state` naturally falls on a 16-byte boundary within the struct.

When a new process is created, we copy the clean template into that process's `fpu_state` field with the kernel's integer-only memcpy routine. Just before we hand control to the new process for the first time, we issue an `fxrstor` instruction, which reads 512 bytes from the process's `fpu_state` field and loads the FPU and SSE registers. From that point on, any floating-point computation the user program performs is backed by hardware registers that carry its own values.

#### Context-Switch Integration on x86

Our scheduler wires `fxsave` and `fxrstor` into the context-switch path. When the timer tick decides to preempt the currently running process, the outgoing process's FPU state is written to its `fpu_state` field with `fxsave`, and the incoming process's state is loaded with `fxrstor` before control returns to its user code. Because all of the plumbing is already in place from this chapter, the scheduler's floating-point handoff reduces to "save the old image, restore the new image" at each switch.

#### Keeping Kernel C Out of the FPU

Enabling SSE at the hardware level does not mean ordinary kernel C should use it. **GCC** (GNU C Compiler, the compiler this project uses) is told not to emit SSE, MMX, or x87 floating-point instructions for kernel C via `-mno-sse -mno-sse2 -mno-mmx -msoft-float`. That keeps the kernel from silently clobbering the `XMM` registers that belong to the currently running user process between explicit `fxsave` and `fxrstor` points.

This policy is stricter than simply enabling SSE2 globally. It prevents bugs where a harmless-looking struct copy in an interrupt path becomes compiler-generated `movdqu`/`movups` instructions and mutates process FPU state before the scheduler has saved it. Until we have a separate FPU ownership protocol for our own vector work, SSE usage stays explicit and local to the low-level save/restore code.

### On AArch64: Enabling FP/SIMD via `CPACR_EL1`

AArch64 uses a different gating mechanism. At reset, the architecture leaves FP and SIMD access controlled by a single field in a system register. Before any floating-point or SIMD instruction can run at EL0 or EL1 — even a simple `fadd` — the kernel must explicitly open that gate.

The register is **`CPACR_EL1`** (Coprocessor Architectural Control Register at EL1, the AArch64 control register that gates access to FP/SIMD and SVE for EL0/EL1 code). Its **FPEN** field (the field in `CPACR_EL1` that controls FP/SIMD trap behaviour) sits at bits 20–21. When FPEN is `0b00` (the reset value), any FP or SIMD instruction at EL0 or EL1 traps to EL1 as an `FP_ACCESS` exception. Writing `0b11` to FPEN tells the CPU to allow FP/SIMD execution at both EL0 and EL1 without trapping.

On the arm64 boot path the kernel sets FPEN to `0b11` as part of early per-CPU initialisation, immediately after the exception vector table is installed and before the scheduler starts. That one write — a single `msr CPACR_EL1, x0` instruction — is the entire gate-opening dance for FP/SIMD on this architecture.

**SVE** (Scalable Vector Extension, the AArch64 wide-vector instruction set that supports register widths from 128 to 2048 bits) is a separate capability with its own trap control in `CPACR_EL1` and in `ZCR_EL1`. In the first slice, SVE is left disabled: we raise FPEN but leave the SVE trap bit alone, so any SVE instruction continues to take a trap. Extending the kernel to manage SVE state is deferred to a later milestone.

*On AArch64 (planned, milestone 4): the per-process FP save/restore path mirrors the x86 FXSAVE template using the FP/SIMD register file.*

### Where the Machine Is by the End of Chapter 6

Both architectures can now run FP/SIMD code without faulting. On x86, the CR0/CR4 dance is complete: the kernel holds a 512-byte clean FPU template in memory, each `process_t` carries a 16-byte-aligned private `fpu_state` buffer, and the scheduler saves and restores FPU state with `fxsave`/`fxrstor` at every context switch. On AArch64, writing `0b11` to the FPEN field of `CPACR_EL1` opens the gate for FP/SIMD execution at EL0 and EL1, with SVE kept disabled in this slice and the per-process save/restore path planned for milestone 4.

The important thing to hold in your head going into Chapter 7 is that the CPU itself is now fully configured — GDT, IDT, PIC, IRQ dispatch, FP/SIMD are all owned by the kernel. Part I ends here with the machine fully governable. Part II starts with the next big question: now that the CPU is ours, how much RAM is actually out there, and how do we keep track of which pages are in use? Chapter 7 covers the first half of that answer — discovering what memory the firmware believes exists — so Chapter 8 can build the managers that hand it out.

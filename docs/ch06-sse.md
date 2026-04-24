\newpage

## Chapter 6 — SSE2 and CPU Extensions

### An Unconfigured CPU Cannot Run Modern Code

Chapter 5 left the interrupt dispatch registry in place and the wall clock ticking. Before we create any processes, there is a piece of hardware setup that must happen first: enabling the floating-point and vector extensions the CPU keeps locked off by default.

When the CPU comes out of reset it supports an old, conservative subset of the x86 instruction set. Any modern extension — the floating-point unit, the vector instructions, the 128-bit and 256-bit registers — is turned off and must be explicitly enabled by the operating system. The CPU does this to protect the OS from legacy software that does not know how to save and restore the extra registers across context switches. It is like a multitasking office where employees share one big desk. If the desk has hidden drawers that only some employees know about, and one forgets to close a drawer, the next employee may overwrite what is inside. So the OS keeps the drawers locked until the scheduler promises to manage them.

The particular extension this chapter enables is **SSE2** (Streaming SIMD Extensions 2), a family of instructions introduced by Intel in 2000. **SIMD** (Single Instruction, Multiple Data) is the class of instruction that operates on several values simultaneously — the canonical example being "add four 32-bit floats at the same time". SSE2 provides this capability through eight 128-bit registers named `XMM0` through `XMM7`, each wide enough to hold four 32-bit floats, two 64-bit doubles, or various combinations of integer types. Modern compilers can use SSE2 aggressively even for code that looks like ordinary C, but we deliberately disable compiler-generated SSE for kernel C so that only explicit low-level FPU/SSE ownership code touches those registers.

If we do not enable SSE2 before the first explicit `fxsave` or `fxrstor` executes, the CPU raises an **Invalid Opcode exception** (`#UD`, short for "undefined instruction"). Our handler from Chapter 4 would catch the fault, print an error, and halt. This is exactly what happens if you try to manage per-process FPU state without the setup described here.

We do this work before any process is created because the hardware must be configured before a user program runs. A program that encounters floating-point operations before `OSFXSR` is set will fault on its first such instruction — there is no safe way to fix that mid-flight.

### The Four Control Bits That Gate SSE

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

### Capturing a Clean FPU Snapshot

Once the control bits are in place, we initialise the floating-point unit with the `fninit` instruction (which resets the x87 FPU — the legacy floating-point unit present since the 80387 — to a well-defined state: all exceptions masked, double-extended precision, stack pointer zeroed) and then use `fxsave` (FP state eXtended save) to dump the complete FPU and SSE register state into a 512-byte buffer. That buffer becomes the **template** every new process is initialised from.

The template contains these well-known values:

- `FCW = 0x037F` — the FPU control word, with all x87 floating-point exceptions masked.
- `FSW = 0` — the status word, with no exceptions pending.
- `MXCSR = 0x1F80` — the SSE control register, with all SSE exceptions masked and rounding set to nearest.
- All eight `XMM` registers zeroed.

Any process created from this template starts life with a clean, predictable floating-point environment.

### Giving Every Process Its Own FPU State

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

When a new process is created, we copy the clean template into that process's `fpu_state` field with the kernel's integer-only memcpy routine.

Just before we hand control to the new process for the first time, we issue an `fxrstor` instruction, which reads 512 bytes from the process's `fpu_state` field and loads the FPU and SSE registers. From that point on, any floating-point computation the user program performs is backed by hardware registers that carry its own values.

### Context-Switch Integration

Our scheduler wires `fxsave` and `fxrstor` into the context-switch path. When the timer tick decides to preempt the currently running process, the outgoing process's FPU state is written to its `fpu_state` field with `fxsave`, and the incoming process's state is loaded with `fxrstor` before control returns to its user code. Because all of the plumbing is already in place from this chapter, the scheduler's floating-point handoff reduces to "save the old image, restore the new image" at each switch.

### Keeping Kernel C Out of the FPU

Enabling SSE at the hardware level does not mean ordinary kernel C should use it. **GCC** (GNU C Compiler, the compiler this project uses) is told not to emit SSE, MMX, or x87 floating-point instructions for kernel C via `-mno-sse -mno-sse2 -mno-mmx -msoft-float`. That keeps the kernel from silently clobbering the `XMM` registers that belong to the currently running user process between explicit `fxsave` and `fxrstor` points.

This policy is stricter than simply enabling SSE2 globally. It prevents bugs where a harmless-looking struct copy in an interrupt path becomes compiler-generated `movdqu`/`movups` instructions and mutates process FPU state before the scheduler has saved it. Until we have a separate FPU ownership protocol for our own vector work, SSE usage stays explicit and local to the low-level save/restore code.

### Where the Machine Is by the End of Chapter 6

The CPU is now willing to execute explicit SSE2 save/restore instructions without faulting. That single change unlocks the rest of the floating-point story: we have a 512-byte template of a clean FPU state sitting in kernel memory, ready to be copied into any new process, and the `process_t` struct carries a private FPU state buffer at a 16-byte-aligned offset that the scheduler will save and restore on every context switch. Kernel C is deliberately kept off SSE, MMX, and x87, so process FPU state is only ever changed at explicit save/restore boundaries and nowhere else.

The important thing to hold in your head going into Chapter 7 is that the CPU itself is now fully configured — GDT, IDT, PIC, IRQ dispatch, FPU, and SSE are all owned by the kernel. Part I ends here with the machine fully governable. Part II starts with the next big question: now that the CPU is ours, how much RAM is actually out there, and how do we keep track of which pages are in use? Chapter 7 covers the first half of that answer — discovering what memory the firmware believes exists — so Chapter 8 can build the managers that hand it out.

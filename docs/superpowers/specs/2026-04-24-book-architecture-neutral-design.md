# Architecture-Neutral Book Update — Design

## Goal

Update the Drunix book so it reads as a single narrative covering both x86 and AArch64, rather than an x86 narrative with an AArch64 appendix. Where a topic generalizes across architectures, the book explains the OS concept first, then folds in per-architecture mechanics. Where a topic is genuinely architecture-specific, the book stays single-arch and points at the analogue (or absence of one) on the other architecture.

The book operates at the eventual-parity timeline: subsystems that exist on x86 but have not yet landed on arm64 are still discussed at the concept level today, with arm64 specifics behind a "planned, milestone N" marker that is removed when the implementation lands.

## Non-goals

- This work does not touch source code under `boot/`, `kernel/`, `user/`, or the build system. It is a documentation pass only.
- It does not advance the arm64 port itself. Milestone definitions in `docs/arm64-port-plan.md` are unchanged.
- It does not introduce new visual styling for callouts. Plain Markdown only — no new Lua filters, LaTeX macros, or EPUB CSS.

## Approach

### Architecture-neutral framing

Every chapter that covers a topic shared between x86 and arm64 is reframed around the OS concept first. The mechanism narration then describes how each architecture realizes that concept. This matches the book's existing style rule 1 ("narrate the runtime flow") — the runtime is now described in terms of "what the CPU is doing" without assuming which CPU.

### Form of architecture differences

Three concrete forms cover every case. The form is chosen per difference, not per chapter.

- **Inline prose** — when the difference is one or two sentences. Example: "loaded with `lidt` on x86, with a write to `VBAR_EL1` on arm64."
- **Per-arch subsections** — when the difference is structurally large (multiple paragraphs, distinct mechanisms). Standard `###` subsection headings: `### On x86` and `### On AArch64`, in that order.
- **Planned marker** — when the arm64 implementation has not yet landed. Italicized clause: *"On AArch64 (planned, milestone N): …"*. The marker is removed when the milestone lands.

Cross-reference callouts for topics that exist on only one architecture take the form of a single italicized sentence at the relevant location: *"AArch64 has no segmentation; see chXX for the analogous CPU-init step."*

No new styled-box construct is introduced. The PDF and EPUB pipelines render plain Markdown today, and the existing book voice already accommodates italicized asides.

## Chapter treatment

The 31 existing chapters fall into four groups by treatment.

### Group 1 — single-arch with cross-reference callout

Concepts whose mechanism is genuinely architecture-specific. The chapter stays as it is today, with a short cross-reference callout pointing at the analogue (or its absence) on the other architecture.

- **ch02 protected mode / GDT.** Segmentation has no arm64 equivalent. Add a callout near the chapter opening, of the shape: *"AArch64 has no segmentation; the analogous CPU-init step is the EL2→EL1 demotion covered in ch01."*
- **VGA-hardware portions of ch03.** The `0xB8000` text buffer and VGA register interface are PC-specific. The "first console output" framing of ch03 generalizes (Group 2 treatment for that part); the VGA-buffer specifics stay x86-only with a callout that arm64 reaches a comparable text grid through a framebuffer console.

### Group 2 — reframed arch-neutrally with per-arch mechanics

Concepts that generalize cleanly across architectures.

- **ch01 boot handoff.** Concept: firmware delivers control with some state established. x86 (GRUB + Multiboot1, kernel at 1 MB, 32-bit protected mode pre-enabled) and arm64 (QEMU `raspi3b` loader at `0x80000`, four cores in EL2) appear as the two cases. The CPU-state-on-entry diagram becomes one-per-arch.
- **ch03 first console output** (the non-VGA-specific framing). Concept: the kernel's first reliable output path. x86 VGA text buffer vs. arm64 BCM2835 mini-UART. The VGA hardware specifics remain Group 1.
- **ch04 interrupt table.** Concept: the CPU consults a fixed-position table on exception or IRQ. x86 IDT (loaded with `lidt`) vs. arm64 vector table (base in `VBAR_EL1`, 16 fixed slots). Subsection per arch — the table shapes are structurally different enough to warrant it.
- **ch05 IRQ dispatch and timer tick.** Concept: a periodic interrupt drives the scheduler heartbeat. x86 8259 PIC + 8253/4 PIT vs. arm64 BCM2836 core-local interrupt block + ARM Generic Timer. Subsection per arch.
- **ch06 FP/vector enablement.** Retitle around the concept (current title is x86-mechanism-named: "SSE"). x86 SSE2 control-register dance vs. arm64 FPEN bits in `CPACR_EL1`. The "saved FP context template every process inherits" framing is shared.
- **ch07 memory detection.** Concept: how the kernel learns what physical RAM exists. x86 Multiboot memory map vs. arm64 fixed BCM2837 layout (DTB parsing left as future work, per `arm64-port-plan.md`).
- **ch08 memory management, ch25 demand paging, ch26 copy-on-write fork.** Paging concepts. x86 IA-32 two-level page tables, `cr3`, `invlpg` vs. arm64 four-level translation, `TTBR0_EL1`/`TTBR1_EL1`, `TLBI`. arm64 specifics behind the "planned, milestone 3" marker until the MMU bring-up lands.
- **ch15 processes.** Concept: per-process kernel state and context switching. x86 `ESP`/`CR3`/`iret` save and restore vs. arm64 `x19`–`x29`, `LR`, `SP`, `TTBR0` save and restore. Most arm64 details behind "planned, milestone 4."
- **ch16 syscalls.** x86 `int 0x80` (vector 128) vs. arm64 `svc #0` (arguments in `x0`–`x5`, number in `x8`). Subsection per arch.
- **ch17 file I/O, ch19 signals, ch20 user runtime, ch24 core dumps.** ABI-shaped concepts. The shared concept narration is unchanged; per-arch subsections cover trap-frame layout, signal trampoline shape, and core-dump register-set layout. arm64 details behind "planned, milestone 4."
- **ch21 libc.** The libc surface itself is portable C; the arch-specific seams (CRT0, syscall stub) get inline differences. x86 `user/lib/crt0.asm` (32-bit i386 calling convention) vs. arm64 `user/lib/crt0.S` (AArch64 initial stack contract: `argc`, `argv`, `envp`, `auxv`).

### Group 3 — light revision only

Subsystems that are already mostly portable C. The revision removes phrasings that assume "the kernel runs on x86" and adds short callouts where the platform driver underneath differs.

- ch09 klog, ch12 device registries, ch13 filesystem, ch14 VFS, ch18 tty, ch22 shell, ch23 modules, ch29 debugging, ch30 C++ userland — phrasing pass only.
- ch10 keyboard. Concept (input device abstraction) is portable. Driver layer differs: PS/2 on x86 PC vs. arm64 USB HID (planned, milestone after current arm64-port-plan).
- ch11 ATA disk. Concept (block-device interface) is portable. Driver layer differs: PC ATA vs. arm64 SDHCI/EMMC (planned, milestone 5 per `arm64-port-plan.md`).
- ch27 mouse. Same shape as ch10 — concept portable, driver differs (PS/2 mouse vs. USB HID, planned).
- ch28 desktop. Concept (compositor + windowing) is portable C. The presentation layer and input source differ per arch and per platform; arm64 framebuffer planning lands in milestone 6.

### Group 4 — ch31 dissolves

ch31 (AArch64 bring-up) currently exists as a single-chapter alternative narrative for the early boot path. Its content is redistributed:

- EL2→EL1 demotion → ch01 (boot handoff) and ch02 (cross-reference callout).
- Mini-UART bring-up → ch03 (first console output).
- Exception vector table → ch04.
- ARM Generic Timer + BCM2836 core-local interrupt block → ch05.
- BCM2835/2836 platform-specific peripheral programming (mailbox addresses, GPIO alt-function selection, baud-divisor calculation) → a new short appendix chapter, "AArch64 platform notes," at the end of the book. This parallels how PC-specific peripheral details live in their own chapters today (ch10, ch11, ch27).

Renumbering: ch31's content is redistributed, and the new "AArch64 platform notes" appendix replaces it as the new ch31. No mid-book renumbering is required, and `DOCS_SRC` in the Makefile keeps a contiguous chapter list.

### Part intros and front matter

- **`partI-firmware-to-kernel.md`.** Currently entirely x86 narrative. Rewrite arch-neutrally: "firmware delivers control" instead of "GRUB hands the kernel a Multiboot info pointer." x86 and arm64 specifics moved to the chapter-level treatment described above.
- **`partII-memory.md` through `partX-development-tools.md`.** Lighter touches where they reference x86 specifics in their framing prose. Most part intros already speak at a concept level.
- **`cover.md`, `epub-cover.md`, `epub-copyright.md`.** No architecture content. No change.
- **`docs/arm64-port-plan.md`.** Remove the line "The book chapters `ch01`–`ch30` describe the x86 kernel as-is. They stay authoritative for the x86 target." Replace with one sentence noting that the book is now arch-neutral and that arm64 milestones appear as "planned" markers until they land.

## Diagrams

The `style.md` diagram admission test still applies. New diagrams are added only where the shape genuinely differs and a reader benefits from seeing it.

- **New diagrams expected.** AArch64 vector table 16-slot layout (ch04). AArch64 translation-table walk (ch08 / ch25). AArch64 trap frame on syscall entry (ch15 / ch16). AArch64 initial user stack contract (ch20). One CPU-state-on-entry diagram pair for ch01 (x86 + arm64).
- **Existing diagrams preserved.** All x86-mechanism diagrams (GDT byte layout, IDT entry, PDE/PTE bits, 8259 vector map, etc.) stay in place — they document the x86 path, which remains in the book.
- **Diagram numbering.** New diagrams take new `chNN-diagNN.svg` numbers within their chapter, in first-appearance order, per `style.md`. Where a new diagram is interleaved into an existing chapter's first-appearance order, later diagrams in that chapter are renumbered.
- **Generator updates.** All new diagrams go through `docs/generate_diagrams.py` per the existing workflow. No hand-edited SVGs.

## Verification per chapter

The book has no automated test suite. Verification is:

1. **Build pipelines clean.** `make` in `docs/` produces both PDF and EPUB without warnings.
2. **Diagrams render.** `docs/render_diagram_check.sh` for any chapter whose diagrams changed.
3. **Visual proof-read.** Skim the rendered chapter for: cross-references resolve, callouts read in the book voice, no leftover x86-only phrasing in arch-neutral chapters, no leftover "planned, milestone N" markers for arm64 work that has actually landed.

## Out of scope and risks

- **Speculative arm64 detail drift.** Some arm64 mechanics described as "planned" may shift during implementation. Mitigation: the planned marker keeps the prose honest, and the arm64-port-plan milestones are the source of truth for what is intended.
- **Rule 9 enforcement.** `style.md` rule 9 forbids using a concept before it has been introduced. Reframing chapters arch-neutrally must not introduce arm64 terms (`VBAR_EL1`, `TTBR0_EL1`, `svc`, EL0/EL1, etc.) before they have been explained. The first chapter that introduces each arm64 term gives it the same one-sentence plain-English definition that x86 terms receive today.
- **Chapter ordering.** The pedagogical-ordering rule (`style.md` rule 5) still applies. Reframing a chapter must not cause it to depend on concepts not yet introduced. Where a per-arch subsection would forward-reference, it is deferred to the chapter where the prerequisite is introduced.
- **Renumbering risk is small.** Dissolving ch31 creates a tail-only gap; the appendix replaces it. No mid-book renumbering, so no `Makefile` `DOCS_SRC` reordering past the tail.

## Sequencing

The implementation plan that follows this spec will sequence the work so the book stays buildable at every step:

1. Part-intro and arm64-port-plan phrasing fixes (no chapter content changes).
2. Group 3 phrasing pass (smallest behavioral change per chapter).
3. Group 1 callout insertions.
4. Group 2 chapter-by-chapter rewrites, in pedagogical order (ch01 first, then ch03 first-console split, then ch04, ch05, ch06, ch07, ch08, then process/syscall/signal cluster).
5. ch31 dissolution and appendix creation.
6. Diagram generator updates and renders, interleaved with each chapter that needs them.
7. Final book build and proof-read pass.

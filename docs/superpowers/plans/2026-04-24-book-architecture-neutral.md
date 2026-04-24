# Architecture-Neutral Book Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reframe the Drunix book so it reads as a single narrative covering both x86 and AArch64 — concept first, per-architecture mechanics second — instead of an x86 narrative with an AArch64 appendix.

**Architecture:** Each chapter is reclassified into one of four groups (single-arch with cross-reference, arch-neutral with per-arch mechanics, light revision only, dissolved). Differences are folded inline when short and into `### On x86` / `### On AArch64` subsections when structurally large. Subsystems not yet on arm64 are still discussed at the concept level today, with their arm64 specifics behind a *"On AArch64 (planned, milestone N): …"* marker that is removed when the implementation lands.

**Tech Stack:** Markdown chapter sources under `docs/`, `docs/generate_diagrams.py` for SVG diagrams, pandoc + Typst PDF and pandoc EPUB builds via `make pdf` / `make epub` / `make docs`, plain-Markdown callouts only (no new Lua filters or styling).

---

## Source of Truth

Spec: `docs/superpowers/specs/2026-04-24-book-architecture-neutral-design.md`. Read it before starting. Whenever this plan and the spec disagree, the spec wins — and either fix the plan inline or flag the contradiction.

## Style Rules To Honour Throughout

These come from `docs/style.md` and `docs/contributing/docs.md`. Apply them to every prose edit:

- **Rule 1 — narrate the runtime flow.** Open each rewritten section with what is happening inside the machine right now, not "this chapter adds X."
- **Rule 2 — introduce all terms on first use.** New arm64 terms (`VBAR_EL1`, `TTBR0_EL1`, `svc`, EL0/EL1, `CPACR_EL1`, etc.) get a one-sentence plain-English definition the first time they appear in any chapter.
- **Rule 3 — minimal code blocks.** Prefer prose over pasted code. Allowed: C struct definitions, short register/bit tables, brief inline snippets where syntax is the point.
- **Rule 4 — visual aids for spatial concepts.** New arm64 layouts (vector table, translation walk, trap frame, initial stack) earn diagrams. Run them through `docs/generate_diagrams.py`; verify with `docs/render_diagram_check.sh`.
- **Rule 5 — pedagogical ordering.** Never use a concept before it has been introduced. If a per-arch subsection would forward-reference, defer the detail to a later chapter and leave only a one-sentence pointer.
- **Rule 6 — voice and tone.** First-person plural ("we", "let's"), conversational, end every chapter with "Where the Machine Is by the End of Chapter N" — and that recap acknowledges *both* arches' state.
- **Rule 7 — concepts, not catalogues.** Even when both arches are in view, explain *how* the mechanism works inside, not which flags exist.
- **Rule 9 — no forward use of unintroduced concepts.** Same rule, restated because it bites hardest in this rewrite. Re-introduce briefly rather than deferring.

The three callout forms allowed (per spec):
1. **Inline difference** — plain prose, e.g. "loaded with `lidt` on x86, with a write to `VBAR_EL1` on arm64."
2. **Per-arch subsections** — `### On x86` and `### On AArch64`, in that order, when the difference needs more than two sentences.
3. **Planned marker** — italicized clause woven into the sentence, e.g. *"On AArch64 (planned, milestone 3): …"*.

Cross-reference callouts (Group 1) are a single italicized sentence: *"AArch64 has no segmentation; see ch01 for the analogous CPU-init step."*

No new styled-box construct. No emoji. No HTML.

## Build And Verify

After every chapter touched, run:

```bash
make pdf
```

Expected: build succeeds with no pandoc warnings about missing references or unresolved images. If a diagram changed, also run:

```bash
docs/render_diagram_check.sh
```

Final task runs `make docs` (PDF + EPUB) and a visual proof-read.

## Commit Discipline

One commit per chapter (or per logical edit set within a task). Commit messages follow `docs/contributing/commits.md`. Do not batch unrelated edits.

---

## File Structure

**Chapter files modified (`docs/`):**

- `partI-firmware-to-kernel.md` — arch-neutral rewrite of Part I intro
- `partII-memory.md`, `partIII-hardware-interfaces.md`, `partIV-files-and-storage.md`, `partV-running-user-programs.md`, `partVI-user-environment.md`, `partVII-extending-the-kernel.md`, `partVIII-fault-driven-memory.md`, `partIX-graphical-environment.md`, `partX-development-tools.md` — light phrasing-only pass
- `ch01-boot.md` — Group 2 rewrite (boot handoff: GRUB+Multiboot vs. raspi3b loader+EL2)
- `ch02-protected-mode.md` — Group 1 cross-reference callout (no segmentation on arm64)
- `ch03-kernel-entry.md` — split: Group 2 first-console framing + Group 1 VGA hardware specifics
- `ch04-interrupts.md` — Group 2 rewrite (IDT vs. vector table at `VBAR_EL1`)
- `ch05-irq-dispatch.md` — Group 2 rewrite (8259+PIT vs. BCM2836 + Generic Timer)
- `ch06-sse.md` — Group 2 rewrite + retitle (FP/vector enablement)
- `ch07-memory-detection.md` — Group 2 rewrite (Multiboot memmap vs. fixed BCM2837 layout)
- `ch08-memory-management.md` — Group 2 rewrite (IA-32 2-level vs. AArch64 4-level, planned arm64 detail)
- `ch09-klog.md`, `ch12-device-registries.md`, `ch13-filesystem.md`, `ch14-vfs.md`, `ch18-tty.md`, `ch22-shell.md`, `ch23-modules.md`, `ch29-debugging.md`, `ch30-cpp-userland.md` — Group 3 phrasing pass
- `ch10-keyboard.md`, `ch11-ata-disk.md`, `ch27-mouse.md`, `ch28-desktop.md` — Group 3 phrasing pass + planned-driver callouts
- `ch15-processes.md`, `ch16-syscalls.md`, `ch17-file-io.md`, `ch19-signals.md`, `ch20-user-runtime.md`, `ch21-libc.md`, `ch24-core-dumps.md`, `ch25-demand-paging.md`, `ch26-copy-on-write-fork.md` — Group 2 rewrites with arm64 details behind planned markers
- `ch31-aarch64-bringup.md` — content redistributed and replaced with the new "AArch64 Platform Notes" appendix (same filename to keep `DOCS_SRC` contiguous)

**Generator and diagrams:**

- `docs/generate_diagrams.py` — add definitions for new arm64 diagrams (CPU-state-on-entry pair, vector table 16-slot layout, translation-table walk, trap frame, initial user stack)
- `docs/diagrams/*.svg` — new SVGs generated by the script

**Other:**

- `docs/arm64-port-plan.md` — remove "ch01–ch30 stay authoritative for the x86 target" language; replace with a sentence noting the book is now arch-neutral
- `docs/sources.mk` — only changed if a chapter file is renamed (it should not be, since ch31 keeps its number); verify after the appendix task

No source code under `boot/`, `kernel/`, `user/` is touched. No `Makefile` changes outside `docs/sources.mk` (and only if a rename happens).

---

## Task 1: Update arm64-port-plan.md and Front Matter

**Files:**
- Modify: `docs/arm64-port-plan.md`

- [ ] **Step 1: Locate the policy line**

The file currently contains a "Relationship to the existing book" section that says ch01–ch30 stay authoritative for the x86 target.

- [ ] **Step 2: Replace the section**

Replace the entire "Relationship to the existing book" section with:

```markdown
## Relationship to the existing book

The book is written arch-neutrally: every chapter explains the OS concept first
and then folds in per-architecture mechanics for both x86 and AArch64.
Subsystems that exist on x86 but have not yet landed on AArch64 are still
discussed at the concept level today, with AArch64 specifics carried under a
*"On AArch64 (planned, milestone N)"* marker that is removed when the
milestone lands.  See `docs/superpowers/specs/2026-04-24-book-architecture-neutral-design.md`
for the design.
```

- [ ] **Step 3: Verify the build**

Run:

```bash
make pdf
```

Expected: PASS with no pandoc warnings.

- [ ] **Step 4: Commit**

```bash
git add docs/arm64-port-plan.md
git commit -m "docs: drop x86-authoritative book policy from arm64 port plan"
```

## Task 2: Rewrite Part I Intro (partI-firmware-to-kernel.md)

**Files:**
- Modify: `docs/partI-firmware-to-kernel.md`

- [ ] **Step 1: Read the current intro**

The current text describes only the x86 path: real mode, GRUB hand-off, Multiboot info, GDT/IDT/PIC/PIT/SSE chapter walkthrough.

- [ ] **Step 2: Rewrite arch-neutrally**

Replace the file contents (keep the `# Part I — From Firmware to Kernel` heading) with prose that:

1. Opens on the universal predicament: when a machine powers on, the CPU is not running our code — firmware runs first, brings hardware to a usable baseline, and hands off to whatever it found to boot. Describe that hand-off arch-neutrally: control reaches the kernel with *some* initial machine state established, and the kernel must finish bringing the CPU into a state where everything else can be built.
2. Names the two starting points the book covers without making one feel primary: an x86 PC where firmware is the BIOS, the bootloader is GRUB, and the CPU enters in 32-bit protected mode with a Multiboot info pointer; and an ARM-based machine (modelled here as the QEMU `raspi3b` Raspberry Pi 3) where firmware loads the kernel image at a fixed physical address and the CPU enters in EL2 with all four cores running.
3. Lists the chapter goals at the *concept* level, not the mechanism level. ch01 follows the firmware-to-kernel hand-off. ch02 covers the x86 segmentation model (with a note that AArch64 has no segmentation). ch03 takes the first reliable output. ch04 installs an exception/interrupt table. ch05 turns periodic hardware events into a scheduler tick. ch06 brings up the floating-point and vector unit.
4. Closes on the same end-state framing as before: by the end of Part I the kernel is in full control of the CPU on either supported architecture.

- [ ] **Step 3: Build and proof-read**

```bash
make pdf
```

Open `docs/Drunix\ OS.pdf` and confirm Part I reads coherently and references no x86-only terms in its concept-level prose.

- [ ] **Step 4: Commit**

```bash
git add docs/partI-firmware-to-kernel.md
git commit -m "docs: rewrite Part I intro arch-neutrally"
```

## Task 3: Light Phrasing Pass on Other Part Intros

**Files:**
- Modify: `docs/partII-memory.md`
- Modify: `docs/partIII-hardware-interfaces.md`
- Modify: `docs/partIV-files-and-storage.md`
- Modify: `docs/partV-running-user-programs.md`
- Modify: `docs/partVI-user-environment.md`
- Modify: `docs/partVII-extending-the-kernel.md`
- Modify: `docs/partVIII-fault-driven-memory.md`
- Modify: `docs/partIX-graphical-environment.md`
- Modify: `docs/partX-development-tools.md`

- [ ] **Step 1: Audit each part intro**

For each file, grep the file for hard x86 terms (`x86`, `i386`, `32-bit`, `IA-32`, `GDT`, `IDT`, `PIC`, `PIT`, `int 0x80`, `cr3`, `CR3`, `ESP`, `EFLAGS`, `protected mode`, `Multiboot`, `GRUB`). Read the surrounding paragraphs.

- [ ] **Step 2: Rewrite per file**

Where an x86 term appears in *concept-level* framing prose (e.g., "the kernel installs the IDT before…"), revise to concept-level wording ("the kernel installs the exception/interrupt table before…") and let the chapter-level text carry the mechanism details. Where a term names a real x86-only chapter topic (e.g., a part intro that lists ch02 protected mode in its tour), keep the x86 term — it is correct for that chapter. Add a single sentence per part intro mentioning that the chapters within are written for both architectures where the topic generalizes.

- [ ] **Step 3: Build and proof-read**

```bash
make pdf
```

Skim each part intro in the rendered PDF.

- [ ] **Step 4: Commit**

```bash
git add docs/partII-memory.md docs/partIII-hardware-interfaces.md \
        docs/partIV-files-and-storage.md docs/partV-running-user-programs.md \
        docs/partVI-user-environment.md docs/partVII-extending-the-kernel.md \
        docs/partVIII-fault-driven-memory.md docs/partIX-graphical-environment.md \
        docs/partX-development-tools.md
git commit -m "docs: phrase part intros arch-neutrally where the topic generalizes"
```

## Task 4: ch01 Boot Handoff — Group 2 Rewrite

**Files:**
- Modify: `docs/ch01-boot.md`
- Modify: `docs/generate_diagrams.py`
- Possibly create: `docs/diagrams/ch01-diagNN.svg` (regenerated)

- [ ] **Step 1: Re-read the chapter and ch31**

`ch01-boot.md` covers GRUB, Multiboot1, where the kernel image lands, framebuffer mode requested, and the linker-script guarantees before the first C instruction. `ch31-aarch64-bringup.md` covers the parallel arm64 hand-off (raspi3b loader at `0x80000`, four cores in EL2, EL2→EL1 demotion, stack pointer set, `.bss` zeroed).

- [ ] **Step 2: Rewrite the chapter opening as the arch-neutral concept**

Replace the chapter's opening section (everything up to but not including the first detailed mechanism section) with arch-neutral framing: firmware delivers control with some machine state already established, and the kernel must take that handoff and complete enough setup to reach C-level execution. Then introduce the two cases the book follows:

- **x86 PC**: GRUB loads the kernel at 1 MB per the linker script, leaves the CPU in 32-bit protected mode with paging off, hands a Multiboot info pointer in `EBX`, and a magic value in `EAX`.
- **AArch64 (Raspberry Pi 3, QEMU `raspi3b`)**: the firmware/loader places the kernel at physical `0x80000`, all four cores enter at that address in AArch64 state at EL2, and there is no Multiboot-equivalent structure — boot info is fixed by the platform.

- [ ] **Step 3: Add per-arch subsections for the mechanism narration**

Use form-2 (per-arch subsections) for the chapter body. Suggested headings inside the chapter:

- `### On x86: from GRUB to the C entry`
  - Reuse the existing GRUB / Multiboot / linker-script / framebuffer-request prose. Ensure terms are introduced per rule 2.
- `### On AArch64: from raspi3b loader to the C entry`
  - Move the EL2 hand-off content from ch31 here: identifying the primary core and parking the others, inspecting and lowering the exception level (with `HCR_EL2`, `SCTLR_EL1`, `ELR_EL2`, and `eret` introduced and defined), seeding the stack pointer, zeroing `.bss`. Drop ch31-only framing (e.g., "the first slice is small") since this chapter is now the canonical home of that material.

- [ ] **Step 4: Update the CPU-state-on-entry diagram**

The existing ch01 state-snapshot diagram covers the x86 hand-off only. Add a sibling AArch64 diagram showing register/EL state at kernel entry on raspi3b:

1. Open `docs/generate_diagrams.py`. Find the existing ch01 state-snapshot definition.
2. Add a parallel definition for the arm64 entry state (cores parked, primary core in EL2, kernel image at `0x80000`, no Multiboot pointer).
3. Renumber later ch01 diagrams to keep first-appearance order contiguous.
4. Run the generator:

```bash
python3 docs/generate_diagrams.py
```

5. Render-check the new SVGs:

```bash
docs/render_diagram_check.sh
```

6. Reference both diagrams in the chapter prose at the points where each arch is described.

- [ ] **Step 5: Update the "Where the Machine Is by the End of Chapter 1" recap**

Acknowledge both arches: the kernel has reached C-level execution with a known stack and zeroed `.bss` on either supported architecture; on x86 the CPU is in 32-bit protected mode with the Multiboot info available, on AArch64 the CPU is in EL1 with the primary core executing alone.

- [ ] **Step 6: Build and proof-read**

```bash
make pdf
```

- [ ] **Step 7: Commit**

```bash
git add docs/ch01-boot.md docs/generate_diagrams.py docs/diagrams/ch01-*.svg
git commit -m "docs(ch01): rewrite boot handoff arch-neutrally"
```

## Task 5: ch02 Protected Mode — Group 1 Cross-Reference Callout

**Files:**
- Modify: `docs/ch02-protected-mode.md`

- [ ] **Step 1: Read the chapter**

The chapter covers the x86 GDT, segment selectors, the flat memory model, and how protected mode constrains memory accesses.

- [ ] **Step 2: Add the cross-reference callout**

Near the chapter opening (after the chapter title and lead paragraph), add a single italicized sentence:

```markdown
*AArch64 has no segmentation; the analogous CPU-init step on that architecture
is the EL2-to-EL1 demotion covered in ch01.*
```

Do **not** rewrite the rest of the chapter — segmentation is x86-only and the existing content is correct.

- [ ] **Step 3: Verify rule 9**

Make sure the callout's terms ("EL2", "EL1", "demotion") have been introduced in ch01. They will have, after Task 4.

- [ ] **Step 4: Build and commit**

```bash
make pdf
git add docs/ch02-protected-mode.md
git commit -m "docs(ch02): note absence of segmentation on arm64"
```

## Task 6: ch03 Kernel Entry — Split Group 1 (VGA) and Group 2 (First Console)

**Files:**
- Modify: `docs/ch03-kernel-entry.md`

- [ ] **Step 1: Read the chapter**

The chapter currently mixes "the kernel produces its first output" with "VGA text buffer at `0xB8000` is how that output happens." The split is:

- The first-output framing (the *concept*) generalizes — every CPU needs a first reliable output path.
- The VGA hardware specifics (memory-mapped text buffer, attribute byte, cursor I/O ports) are PC-specific.

- [ ] **Step 2: Reframe the chapter opening as Group 2**

Rewrite the chapter opening so the lead concept is "the first reliable output," not "VGA." Introduce the two paths:

- **x86 PC**: VGA text buffer at `0xB8000`, attribute byte format, memory-mapped writes.
- **AArch64 (raspi3b)**: BCM2835 mini-UART, register programming sequence (enable aux block, configure 8-bit, set baud divisor, switch GPIO14/15 to alt-fn 5, enable TX/RX), busy-wait on TX-empty.

Use form-2 subsections (`### On x86: VGA text buffer` and `### On AArch64: BCM2835 mini-UART`). Move the mini-UART content from ch31 here.

- [ ] **Step 3: Add the Group 1 callout for the VGA-buffer specifics**

Inside `### On x86: VGA text buffer`, the existing detailed VGA register / cursor / attribute discussion stays. Add a one-sentence callout near the start of that subsection:

```markdown
*This buffer and its attribute model are PC hardware; the AArch64 path reaches
a comparable cell-grid console through a framebuffer text console (see the
AArch64 platform notes in ch31).*
```

- [ ] **Step 4: Update the recap**

"Where the Machine Is by the End of Chapter 3" acknowledges both: the kernel can print to a known-good console on either arch.

- [ ] **Step 5: Build, proof-read, commit**

```bash
make pdf
git add docs/ch03-kernel-entry.md
git commit -m "docs(ch03): split first-console framing from VGA hardware specifics"
```

## Task 7: ch04 Interrupts — Group 2 Rewrite With Vector-Table Diagram

**Files:**
- Modify: `docs/ch04-interrupts.md`
- Modify: `docs/generate_diagrams.py`
- Possibly create: `docs/diagrams/ch04-diagNN.svg`

- [ ] **Step 1: Read the chapter and ch31's exception-vector content**

ch04 currently covers the x86 IDT: gate descriptor format, `lidt`, vector layout. ch31 covers the AArch64 vector table: 2 KB / 16-slot layout at `VBAR_EL1`, four exception classes (sync, IRQ, FIQ, SError) × four contexts (current EL with `SP_EL0`, current EL with `SP_ELx`, lower EL AArch64, lower EL AArch32).

- [ ] **Step 2: Reframe the chapter opening**

Open with the concept: when an exception or interrupt fires, the CPU consults a fixed-position table to decide where to jump. That requirement is universal; the table's shape and the instruction that registers it differ per arch.

- [ ] **Step 3: Add per-arch subsections**

- `### On x86: the Interrupt Descriptor Table`
  - Reuse the existing IDT prose (gate descriptor layout, `lidt`, vector numbering, kernel/user privilege gates).
- `### On AArch64: the exception vector table at `VBAR_EL1``
  - Move the 16-slot layout from ch31. Introduce `VBAR_EL1` and "exception level" on first use. Explain the four × four matrix and why it exists (separating taken-from-kernel vs. taken-from-user, and `SP_EL0` vs. `SP_ELx` cases).

- [ ] **Step 4: Add the vector-table diagram**

1. In `docs/generate_diagrams.py`, add a layout diagram showing the AArch64 vector table as a 4×4 grid (rows: contexts; columns: exception classes), each cell labelled with its 0x80-byte handler offset.
2. Run `python3 docs/generate_diagrams.py`.
3. Render-check with `docs/render_diagram_check.sh`.
4. Reference the new diagram in the AArch64 subsection at the point where the 16-slot layout is introduced.

- [ ] **Step 5: Update the recap**

Both arches now have a fixed table the CPU will consult on the next exception.

- [ ] **Step 6: Build, proof-read, commit**

```bash
make pdf
git add docs/ch04-interrupts.md docs/generate_diagrams.py docs/diagrams/ch04-*.svg
git commit -m "docs(ch04): cover exception tables on both arches"
```

## Task 8: ch05 IRQ Dispatch and Timer Tick — Group 2 Rewrite

**Files:**
- Modify: `docs/ch05-irq-dispatch.md`

- [ ] **Step 1: Read the chapter and ch31's timer content**

ch05 currently covers the x86 8259 PIC remap, vector layout, and the 8253/4 PIT for periodic ticks. ch31 covers the parallel arm64 path: the BCM2836 core-local interrupt block routes the ARM Generic Timer interrupt to core 0; setup is `CNTFRQ_EL0` read, divisor calculation, `CNTP_TVAL_EL0` write, `CNTP_CTL_EL0` enable.

- [ ] **Step 2: Reframe the chapter opening**

Open on the concept: a periodic interrupt drives the kernel's heartbeat, which is what scheduling, time-keeping, and any later preemption depend on. Above the per-arch wiring, the kernel needs (a) a way to register handler functions for individual IRQ sources without each driver knowing the vector layout, and (b) a guaranteed periodic tick.

- [ ] **Step 3: Add per-arch subsections**

- `### On x86: 8259 PIC plus 8253/4 PIT`
  - Reuse existing prose: PIC remap, OCW1/OCW3, EOI handshake, PIT mode 2, divisor calculation, hooked vector.
- `### On AArch64: ARM Generic Timer plus BCM2836 core-local interrupt block`
  - Introduce `CNTFRQ_EL0`, `CNTP_TVAL_EL0`, `CNTP_CTL_EL0`, the ARM Generic Timer concept, and the BCM2836 core-local interrupt block (a Pi 3-specific peripheral that routes per-core interrupts). Explain the periodic reload model.

- [ ] **Step 4: Update the dispatch-registry framing**

The "drivers register handlers without knowing vector layout" framing is portable — keep it arch-neutral and have each per-arch subsection note where its IRQ source plugs into the registry.

- [ ] **Step 5: Update the recap**

Both arches now produce a periodic interrupt that increments a tick counter visible to the rest of the kernel.

- [ ] **Step 6: Build, proof-read, commit**

```bash
make pdf
git add docs/ch05-irq-dispatch.md
git commit -m "docs(ch05): cover periodic timer interrupts on both arches"
```

## Task 9: ch06 FP/Vector Enablement — Group 2 Rewrite + Retitle

**Files:**
- Modify: `docs/ch06-sse.md` (retitle in heading; filename stays for `DOCS_SRC` stability)

- [ ] **Step 1: Read the chapter**

ch06 currently is titled around SSE specifically and walks the x86 control-register dance to enable SSE2.

- [ ] **Step 2: Retitle the chapter heading**

Change the `## Chapter 6 — …` heading to a concept-level title such as `## Chapter 6 — Floating-Point and Vector Enablement`. Do **not** rename the file (`docs/sources.mk` references `ch06-sse.md` and other tooling may too — keep the filename to avoid renumbering).

- [ ] **Step 3: Reframe the opening**

Open on the concept: most modern CPUs ship with floating-point and SIMD instruction sets disabled at boot, both to keep the boot path predictable and so the kernel can opt into the per-task save/restore costs only when needed. Enabling them is a small, arch-specific control-register dance.

- [ ] **Step 4: Add per-arch subsections**

- `### On x86: enabling SSE2`
  - Reuse the existing CR0/CR4 bit dance prose. Capture the clean FXSAVE state template that becomes the per-process FP starting point.
- `### On AArch64: enabling FP/SIMD via `CPACR_EL1``
  - Introduce `CPACR_EL1` and the FPEN field (with a one-sentence definition). Note that on the arm64 boot path the kernel sets `FPEN` to allow EL1/EL0 FP execution, and that SVE is left disabled in the first slice. *On AArch64 (planned, milestone 4): the per-process FP save/restore path mirrors the x86 FXSAVE template using the FP/SIMD register file.*

- [ ] **Step 5: Update the recap**

Both arches can now run FP/SIMD code, and a clean per-process FP starting state is captured (or planned, for arm64) for inheritance by future processes.

- [ ] **Step 6: Build, proof-read, commit**

```bash
make pdf
git add docs/ch06-sse.md
git commit -m "docs(ch06): generalize FP/vector enablement to both arches"
```

## Task 10: ch07 Memory Detection — Group 2 Rewrite

**Files:**
- Modify: `docs/ch07-memory-detection.md`

- [ ] **Step 1: Read the chapter**

ch07 currently covers the x86 path: parse the Multiboot memory map, identify usable RAM ranges, mark holes (BIOS, video, ACPI, kernel image).

- [ ] **Step 2: Reframe the opening**

Open on the concept: before the kernel can manage physical memory, it has to *learn* what physical memory exists. This information comes from outside the CPU — from the firmware or boot platform — and how it arrives differs by arch.

- [ ] **Step 3: Add per-arch subsections**

- `### On x86: the Multiboot memory map`
  - Reuse the existing prose.
- `### On AArch64: a fixed BCM2837 layout`
  - The Pi 3 has a known fixed RAM layout in QEMU's `raspi3b`; the bring-up uses a hard-coded usable range and a separate fixed device region at `0x3F000000`–`0x40000000`. Note that DTB parsing is a future enhancement (tracked in `docs/arm64-port-plan.md`). *On AArch64 (planned, milestone 3): once the MMU bring-up lands, this hard-coded layout is what the PMM consumes via the new arch-supplied usable-RAM hook.*

- [ ] **Step 4: Update the recap and commit**

```bash
make pdf
git add docs/ch07-memory-detection.md
git commit -m "docs(ch07): describe physical memory discovery on both arches"
```

## Task 11: ch08 Memory Management — Group 2 Rewrite With AArch64 Translation Diagram

**Files:**
- Modify: `docs/ch08-memory-management.md`
- Modify: `docs/generate_diagrams.py`
- Possibly create: `docs/diagrams/ch08-diagNN.svg`

- [ ] **Step 1: Read the chapter**

ch08 covers the x86 PMM (bitmap), the kernel heap (slab/freelist), and IA-32 paging: two-level page tables, `cr3`, PDE/PTE bits, `invlpg`.

- [ ] **Step 2: Reframe the opening**

Open on the concept: once the kernel knows what physical RAM exists, it manages that memory in three layers — a physical frame allocator, a kernel heap built on top of it, and a virtual-memory mapping mechanism that the CPU's MMU consults on every access. The first two are portable C; the third is intimately tied to the CPU's MMU shape.

- [ ] **Step 3: Keep PMM and kheap arch-neutral**

These sections describe portable C. Light phrasing pass only — remove any "the kernel" wording that implicitly assumes x86. Where x86-specific reservations were called out (e.g., low-1MB BIOS/VGA holes), make clear those are PC-platform reservations and that *AArch64 has its own fixed reservation set* (kernel image, framebuffer if allocated, MMIO device window).

- [ ] **Step 4: Add per-arch subsections for paging**

- `### On x86: IA-32 two-level page tables`
  - Reuse PDE/PTE bit-field prose; reuse `cr3` and `invlpg` prose. Keep the existing diagrams.
- `### On AArch64: 4 KB granule, 4-level translation`
  - Introduce `TTBR0_EL1`, `TTBR1_EL1`, granule, translation regimes (a one-sentence definition each). Walk: 48-bit VA, 9-bit indices at L0/L1/L2, 12-bit page offset at L3. Introduce `TLBI` (TLB invalidate) as the analogue of `invlpg`. *On AArch64 (planned, milestone 3): the kernel installs identity-style mappings for kernel RAM, a high-half `TTBR1_EL1` mapping, and a device window for `0x3F000000`–`0x40000000`; TLB shootdown is single-core in the current bring-up since cores 1–3 stay parked.*

- [ ] **Step 5: Add the AArch64 translation-walk diagram**

1. In `docs/generate_diagrams.py`, add a lookup/walk diagram for the AArch64 4-level walk: 48-bit VA bit-field bar at the top, four indirection boxes (L0→L1→L2→L3), final 4 KB page.
2. Generate, render-check, reference in the AArch64 paging subsection.

- [ ] **Step 6: Build and commit**

```bash
make pdf
git add docs/ch08-memory-management.md docs/generate_diagrams.py docs/diagrams/ch08-*.svg
git commit -m "docs(ch08): cover paging on x86 and arm64"
```

## Task 12: Group 3 Phrasing Pass — Portable Subsystems

**Files:**
- Modify: `docs/ch09-klog.md`
- Modify: `docs/ch12-device-registries.md`
- Modify: `docs/ch13-filesystem.md`
- Modify: `docs/ch14-vfs.md`
- Modify: `docs/ch18-tty.md`
- Modify: `docs/ch22-shell.md`
- Modify: `docs/ch23-modules.md`
- Modify: `docs/ch29-debugging.md`
- Modify: `docs/ch30-cpp-userland.md`

- [ ] **Step 1: Audit each file**

For each chapter, grep for x86-only phrasing in the *concept-level* prose: phrases that imply "the kernel runs on x86" rather than describing portable behaviour. Keep all platform-specific details that are correct (e.g., klog's debugcon sink is x86-PC-specific — that stays as a noted PC detail).

- [ ] **Step 2: Apply edits per file**

For each chapter:

1. Replace any phrasing like "in this 32-bit kernel" with "in the kernel" or "on either supported architecture" as appropriate.
2. Where the chapter mentions a peripheral or mechanism whose underlying driver differs by arch, add an inline note. For ch09, klog's debugcon path is x86-only; add: "On AArch64 (planned, milestone 2 of the arm64 port): klog's debug sink uses the mini-UART rather than the QEMU debugcon port."
3. Do not invent new arm64 detail — keep notes brief.

- [ ] **Step 3: Build and proof-read each chapter**

```bash
make pdf
```

- [ ] **Step 4: Commit per chapter**

Commit each chapter separately with a message of the form `docs(chNN): phrase <topic> arch-neutrally`.

## Task 13: Group 3 Phrasing Pass — Platform-Driver Chapters

**Files:**
- Modify: `docs/ch10-keyboard.md`
- Modify: `docs/ch11-ata-disk.md`
- Modify: `docs/ch27-mouse.md`
- Modify: `docs/ch28-desktop.md`

- [ ] **Step 1: Phrasing pass**

These chapters describe a portable concept (keyboard input, block storage, mouse input, desktop) plus a PC-platform driver realization. Keep the PC driver content unchanged; reframe the chapter opening to acknowledge the arch split.

- [ ] **Step 2: Add planned-driver callouts**

- ch10 keyboard: `*On AArch64 (planned): USB HID in place of the PS/2 controller.*`
- ch11 ATA disk: `*On AArch64 (planned, milestone 5): SDHCI/EMMC in place of the PC ATA controller; the block layer interface is unchanged.*`
- ch27 mouse: `*On AArch64 (planned): USB HID in place of the PS/2 mouse.*`
- ch28 desktop: `*On AArch64 (planned, milestone 6): the same compositor and rendering reused once the framebuffer presentation path and an arm64 input device land.*`

- [ ] **Step 3: Build, proof-read, commit per chapter**

```bash
make pdf
git add docs/ch10-keyboard.md
git commit -m "docs(ch10): note planned arm64 USB HID driver"
# repeat per chapter
```

## Task 14: ch15 Processes — Group 2 Rewrite

**Files:**
- Modify: `docs/ch15-processes.md`

- [ ] **Step 1: Read the chapter**

ch15 covers per-process kernel state, context switch (saving `ESP`/`CR3` and using `iret`), and the launch path that drops to user mode.

- [ ] **Step 2: Reframe the opening**

Concept: a process is a kernel-tracked execution context — register file, address-space root, kernel stack, scheduler bookkeeping. Switching between processes is an arch-specific dance over a mostly arch-neutral data structure.

- [ ] **Step 3: Per-arch subsections**

- `### On x86: ESP, CR3, iret`
  - Reuse existing prose.
- `### On AArch64: callee-saved registers, `TTBR0_EL1`, `eret``
  - *On AArch64 (planned, milestone 4): the context switch saves `x19`–`x29`, `LR`, `SP`, and updates `TTBR0_EL1` for the incoming address space. First entry to userland uses `eret` after seeding `ELR_EL1` and `SPSR_EL1`.* Introduce `ELR_EL1`, `SPSR_EL1`, and `eret` per rule 2.

- [ ] **Step 4: Build and commit**

```bash
make pdf
git add docs/ch15-processes.md
git commit -m "docs(ch15): cover process model on both arches"
```

## Task 15: ch16 Syscalls — Group 2 Rewrite With AArch64 Trap-Frame Diagram

**Files:**
- Modify: `docs/ch16-syscalls.md`
- Modify: `docs/generate_diagrams.py`

- [ ] **Step 1: Read the chapter**

ch16 covers the syscall dispatcher: vector 128 (`int 0x80`) on x86, arguments in registers, trap frame on the kernel stack, dispatch table.

- [ ] **Step 2: Reframe the opening**

Concept: a syscall is a deliberate, controlled CPU-supported transition from user code to a known kernel entry point, carrying arguments through registers and producing a return value (and possibly an errno) the same way. The mechanism — which instruction triggers it, where the arguments live, what the trap frame looks like — is arch-specific.

- [ ] **Step 3: Per-arch subsections**

- `### On x86: int 0x80 and the i386 trap frame`
  - Reuse existing prose.
- `### On AArch64: svc #0 and the AArch64 trap frame`
  - Introduce `svc` and `ESR_EL1` per rule 2. Arguments in `x0`–`x5`, syscall number in `x8`. *On AArch64 (planned, milestone 4): the trap frame on syscall entry preserves `x0`–`x30`, `SP_EL0`, `ELR_EL1`, `SPSR_EL1`.* Note the dispatch table itself is shared C — only entry/return assembly differs per arch.

- [ ] **Step 4: Add the AArch64 trap-frame diagram**

1. Layout diagram of the AArch64 syscall trap frame on the kernel stack.
2. Generate, render-check, reference.

- [ ] **Step 5: Build and commit**

```bash
make pdf
git add docs/ch16-syscalls.md docs/generate_diagrams.py docs/diagrams/ch16-*.svg
git commit -m "docs(ch16): cover syscalls on both arches"
```

## Task 16: ch17 File I/O — Group 2 Rewrite

**Files:**
- Modify: `docs/ch17-file-io.md`

- [ ] **Step 1: Read the chapter**

ch17 covers the user-visible file syscalls (`open`, `read`, `write`, `close`, etc.) and their kernel-side implementation through the per-process fd table and VFS.

- [ ] **Step 2: Phrasing pass**

The fd table and VFS are portable C — the chapter is mostly already arch-neutral. The only arch detail is the trap-frame argument layout, which is now in ch16. Keep ch17 focused on the syscall semantics and remove any references that assume an i386 calling convention.

- [ ] **Step 3: Build and commit**

```bash
make pdf
git add docs/ch17-file-io.md
git commit -m "docs(ch17): keep file-IO chapter arch-neutral"
```

## Task 17: ch19 Signals — Group 2 Rewrite

**Files:**
- Modify: `docs/ch19-signals.md`

- [ ] **Step 1: Read the chapter**

ch19 covers signal queueing, delivery, the per-signal disposition table, the signal trampoline (current path is i386 specific), and `sigreturn`.

- [ ] **Step 2: Reframe the opening**

Concept: a signal is the kernel's way to inject control flow into a user process at a controlled boundary. Queueing and disposition are portable; delivery requires the kernel to forge a trap-frame-shaped state on the user stack so that on return the user resumes inside the handler with a `sigreturn` path back to the original frame.

- [ ] **Step 3: Per-arch subsections for delivery**

- `### On x86: signal trampoline and sigreturn`
  - Reuse existing prose.
- `### On AArch64: signal trampoline and sigreturn`
  - *On AArch64 (planned, milestone 4): the signal trampoline forges an AArch64 trap frame on the user stack and the trampoline's `svc #0` to `sigreturn` restores it.* Note this is structurally the same shape as on x86, just over the AArch64 register set.

- [ ] **Step 4: Build and commit**

```bash
make pdf
git add docs/ch19-signals.md
git commit -m "docs(ch19): cover signal delivery on both arches"
```

## Task 18: ch20 User Runtime — Group 2 Rewrite With Initial-Stack Diagram

**Files:**
- Modify: `docs/ch20-user-runtime.md`
- Modify: `docs/generate_diagrams.py`

- [ ] **Step 1: Read the chapter**

ch20 covers what the kernel sets up before the first instruction of a user program runs: the initial user stack (`argc`, `argv`, `envp`, `auxv`), the entry register state, and the CRT0 hand-off.

- [ ] **Step 2: Per-arch subsections**

- `### On x86: i386 initial user stack`
  - Reuse existing prose.
- `### On AArch64: AArch64 initial user stack`
  - *On AArch64 (planned, milestone 4): the initial stack contract follows the AAPCS64 process startup convention — `argc` followed by the `argv` pointer array, `envp` array, and `auxv` entries.*

- [ ] **Step 3: Add the AArch64 initial-stack diagram**

1. Layout diagram of the AArch64 initial user stack.
2. Generate, render-check, reference.

- [ ] **Step 4: Build and commit**

```bash
make pdf
git add docs/ch20-user-runtime.md docs/generate_diagrams.py docs/diagrams/ch20-*.svg
git commit -m "docs(ch20): cover initial user runtime on both arches"
```

## Task 19: ch21 libc — Group 2 Rewrite

**Files:**
- Modify: `docs/ch21-libc.md`

- [ ] **Step 1: Read the chapter**

ch21 covers the libc surface (string, stdio, malloc, etc.) and the small arch-specific seam: CRT0 + the syscall stub.

- [ ] **Step 2: Inline difference**

Almost all of libc is portable C and stays unchanged. Where the chapter discusses CRT0 and the syscall stub, replace x86-only phrasing with inline differences:

- "CRT0 is `user/lib/crt0.asm` on x86 (32-bit i386 calling convention) and `user/lib/crt0.S` on AArch64 (AArch64 calling convention)."
- "The syscall stub uses `int 0x80` on x86 and `svc #0` on AArch64; arguments and return value follow each arch's calling convention."

- [ ] **Step 3: Build and commit**

```bash
make pdf
git add docs/ch21-libc.md
git commit -m "docs(ch21): note arch-specific libc seams"
```

## Task 20: ch24 Core Dumps — Group 2 Rewrite

**Files:**
- Modify: `docs/ch24-core-dumps.md`

- [ ] **Step 1: Read the chapter**

ch24 covers how the kernel produces an ELF core file for a crashed process: register-set snapshot, memory regions, NT_PRSTATUS shape.

- [ ] **Step 2: Per-arch subsections for the register-set snapshot**

- `### On x86: i386 register set in NT_PRSTATUS`
- `### On AArch64: AArch64 register set in NT_PRSTATUS`
  - *On AArch64 (planned, milestone 4): the per-arch register layout follows the AArch64 ELF ABI — `x0`–`x30`, `SP`, `PC`, `PSTATE`.*

- [ ] **Step 3: Build and commit**

```bash
make pdf
git add docs/ch24-core-dumps.md
git commit -m "docs(ch24): cover core-dump register sets per arch"
```

## Task 21: ch25 Demand Paging — Group 2 Rewrite

**Files:**
- Modify: `docs/ch25-demand-paging.md`

- [ ] **Step 1: Read the chapter**

Demand paging covers fault classification and on-demand page materialization through the kernel's page-fault handler.

- [ ] **Step 2: Reframe arch-neutrally**

The fault-classification logic is portable C. Replace any IA-32-specific page-fault-frame phrasing with arch-neutral wording, and add one inline note on the per-arch fault-info source: "the fault address comes from `CR2` on x86 and from `FAR_EL1` on AArch64; the cause bits come from the IA-32 error code or `ESR_EL1` respectively." Introduce `FAR_EL1` and `ESR_EL1` per rule 2 (one-sentence definitions).

*On AArch64 (planned, milestone 3): demand paging is enabled once the AArch64 MMU and PMM bring-up land.*

- [ ] **Step 3: Build and commit**

```bash
make pdf
git add docs/ch25-demand-paging.md
git commit -m "docs(ch25): cover demand paging arch-neutrally"
```

## Task 22: ch26 Copy-On-Write Fork — Group 2 Rewrite

**Files:**
- Modify: `docs/ch26-copy-on-write-fork.md`

- [ ] **Step 1: Reframe arch-neutrally**

CoW fork is mostly portable: page-table walk to mark pages read-only, fault handler to clone on write. The only arch detail is which PTE bit is read-only and how invalidation propagates.

Add one inline note: "The read-only marker is the inverse of the writable PTE bit on x86 and the AP bits in the AArch64 page-table entry on AArch64; invalidation is `invlpg` on x86 and `TLBI` on AArch64." Introduce `TLBI` if not yet introduced in ch08.

*On AArch64 (planned, milestone 3): CoW fork is enabled once the AArch64 MMU bring-up lands.*

- [ ] **Step 2: Build and commit**

```bash
make pdf
git add docs/ch26-copy-on-write-fork.md
git commit -m "docs(ch26): cover copy-on-write fork arch-neutrally"
```

## Task 23: ch31 Dissolution and AArch64 Platform Notes Appendix

**Files:**
- Modify: `docs/ch31-aarch64-bringup.md` (rewrite in place — keep filename and `DOCS_SRC` entry)

- [ ] **Step 1: Confirm content has been redistributed**

Before this task, the following ch31 content should already have been moved into earlier chapters:

- EL2→EL1 demotion → ch01 (Task 4)
- mini-UART bring-up → ch03 (Task 6)
- exception vector table → ch04 (Task 7)
- ARM Generic Timer + BCM2836 core-local interrupt block → ch05 (Task 8)

Open ch31 and verify each of those sections is now redundant. If anything is missing from the destination chapter, fix that chapter first.

- [ ] **Step 2: Replace ch31 with the AArch64 Platform Notes appendix**

Rewrite `docs/ch31-aarch64-bringup.md` end-to-end. The new chapter:

1. Title: `## Chapter 31 — AArch64 Platform Notes`
2. Opens with one paragraph framing this chapter as a per-platform reference for the Pi 3 (BCM2837) and QEMU `raspi3b` peripherals — the same role that the PC-specific peripheral chapters (ch10, ch11, ch27) play for x86.
3. Sections (kept short — concept-level prose with concrete addresses where they teach something):
   - **BCM2835 mini-UART**: physical address, register map summary, baud-divisor formula, GPIO14/15 alt-fn 5 selection.
   - **BCM2836 core-local interrupt block**: physical address, per-core interrupt routing, the timer-interrupt enable for core 0.
   - **VideoCore mailbox property interface (`0x3F00B880`)**: brief overview of the property channel, used by the framebuffer bring-up. *Planned as part of milestone 6.*
   - **Pi 3 fixed memory layout**: usable RAM range, device window `0x3F000000`–`0x40000000`, kernel image at `0x80000`.
   - **Boot entry contract**: what the QEMU `raspi3b` loader guarantees on entry (covered earlier in ch01; this section is a one-paragraph cross-reference and a short list of register/state at entry).
4. Ends with a one-sentence "Where the Machine Is" recap that frames this appendix as background for arm64 readers — no new state established here.

- [ ] **Step 3: Update cross-references**

Search the rest of the book for any remaining reference to "ch31" content that mentioned EL2 demotion, exception vectors, the timer setup, or the mini-UART bring-up. Those should now point to ch01/ch03/ch04/ch05. The only legitimate ch31 references left are to the platform-notes content (mailbox interface, Pi 3 memory layout, BCM2836 core-local interrupt block as a *peripheral*).

```bash
grep -n "ch31\|aarch64-bringup\|Chapter 31" docs/*.md
```

Audit the matches; fix any that point to relocated content.

- [ ] **Step 4: Verify `docs/sources.mk` is unchanged**

```bash
cat docs/sources.mk
```

Expected: unchanged. The filename `ch31-aarch64-bringup.md` stays.

- [ ] **Step 5: Build, proof-read, commit**

```bash
make pdf
git add docs/ch31-aarch64-bringup.md docs/*.md
git commit -m "docs(ch31): replace bring-up chapter with AArch64 platform notes appendix"
```

## Task 24: Final Build, EPUB, And Proof-Read

**Files:**
- No new files.

- [ ] **Step 1: Full doc build**

```bash
make docs
```

Expected: PASS for both PDF (`docs/Drunix\ OS.pdf`) and EPUB (`docs/Drunix\ OS.epub`).

- [ ] **Step 2: Diagram inventory check**

```bash
ls docs/diagrams/*.svg | wc -l
```

Open the PDF and skim each chapter's diagrams. Any new arm64 diagram (CPU-state-on-entry pair in ch01, vector table in ch04, AArch64 translation walk in ch08, AArch64 trap frame in ch16, AArch64 initial user stack in ch20) should render cleanly with no clipped labels.

- [ ] **Step 3: Front-to-back proof-read**

Skim the rendered PDF front to back, checking specifically:

1. No part intro or chapter introduces an arm64 term (`VBAR_EL1`, `TTBR0_EL1`, `svc`, `ELR_EL1`, `SPSR_EL1`, `ESR_EL1`, `FAR_EL1`, `CPACR_EL1`, `TLBI`, EL0/EL1/EL2) before the first chapter that defines it.
2. Every "On AArch64 (planned, milestone N)" marker is accurate against `docs/arm64-port-plan.md`.
3. The "Where the Machine Is" recaps acknowledge both arches where the chapter has been generalized.
4. Cross-references to ch31 point only to the platform-notes content.
5. Voice and tone (rule 6) are preserved throughout.

- [ ] **Step 4: Commit any proof-read fixes**

If any small fixes come out of step 3, commit them as `docs: proof-read pass after architecture-neutral rewrite`.

- [ ] **Step 5: Final smoke commit**

If no fixes are needed, the prior commits are the deliverable. Confirm:

```bash
git log --oneline -30
```

Should show one commit per chapter touched plus the diagram and front-matter commits.

---

## Review Checklist

- [ ] Spec coverage: every Group 1, 2, 3, and 4 chapter in the spec is implemented by a task in this plan.
- [ ] No `kernel/`, `boot/`, `user/`, or `Makefile` files were modified (except `docs/sources.mk`, and only if a rename happened — which the plan deliberately avoids).
- [ ] All new arm64 terms are defined per rule 2 in the first chapter that uses them.
- [ ] All "planned, milestone N" markers reference milestones defined in `docs/arm64-port-plan.md`.
- [ ] All new diagrams come from `docs/generate_diagrams.py` and were render-checked.
- [ ] `make pdf` and `make epub` both succeed.
- [ ] No styled-box or new Lua-filter constructs introduced.
- [ ] `docs/sources.mk` is unchanged from before this plan.

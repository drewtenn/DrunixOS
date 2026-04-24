# ELI5-Spirit Editorial Review

A chapter-by-chapter review of the book aimed at making it more approachable for a working software engineer reading it cold. **Audience is unchanged** — the reader is still a software engineer who has never built an OS (per [docs/style.md](../style.md)). The goal is to find places where the existing prose could ease the reader in more gently, without dumbing down the technical content.

The findings cluster around seven categories:

1. **Cold-start problem** — the chapter drops the reader into dense material without an on-ramp.
2. **Missing everyday analogy** — a concrete real-world analogy would land a concept faster than abstract prose.
3. **Undefined or unscaffolded terms** — a term is used before [docs/style.md](../style.md) rule 9 is satisfied.
4. **Jargon stacking** — three or more OS terms pile up in one sentence without unpacking.
5. **Paragraph overload** — one paragraph crams multiple distinct ideas together.
6. **Missing "why"** — the *what* is explained but not the design rationale.
7. **Weak chapter ending** — the "Where the Machine Is by the End of Chapter N" section is missing, perfunctory, or fails to orient for the next chapter.

---

## Part I — Firmware to Kernel

### ch01-boot
**Overall read**: Clear on-ramp with good narrative flow; introduces firmware and GRUB before diving into binary layout and memory regions.

- [ch01-boot.md:7-18](../ch01-boot.md#L7-L18) — cold-start: opens straight into BIOS vs. UEFI without grounding the reader in *why* we care. Suggest a one-sentence opener: "When you power on a computer, it runs a tiny program baked into the motherboard called firmware. Its job is to find something bootable on the attached disks and hand control to it — a process we call booting."
- [ch01-boot.md:43](../ch01-boot.md#L43) — undefined term on first use: **ELF** is first used shortly after GRUB is discussed but defined only when the entry-point walk begins. Move the one-sentence ELF definition to its first appearance around line 20.
- [ch01-boot.md:59](../ch01-boot.md#L59) — missing "why": we load at 1 MB because "below one megabyte is a patchwork of legacy regions," but the rationale for Multiboot picking *this* address isn't stated. Add: "Multiboot chose 1 MB because it is high enough to avoid firmware regions but low enough to fit on machines with only 4 MB of RAM, the minimum the spec targeted."

### ch02-protected-mode
**Overall read**: Strong structure — real-mode limitations → protected-mode solution → GDT mechanics — with good use of tables for bit layouts.

- [ch02-protected-mode.md:11](../ch02-protected-mode.md#L11) — jargon stacking: "It involves calling BIOS services to read sectors, parsing disk formats, setting up descriptor tables, and flipping the CPU into 32-bit mode." Four concepts stacked with no gloss. Split into two sentences, each introducing one new term with a plain-English definition inline.
- [ch02-protected-mode.md:25-34](../ch02-protected-mode.md#L25-L34) — visual opportunity: the 8-byte descriptor layout with base/limit split across non-contiguous bytes is exactly the "which bytes hold what" problem [docs/style.md](../style.md) rule 4 says to draw rather than describe. The table helps but a packed-format diagram would land faster.
- [ch02-protected-mode.md:75](../ch02-protected-mode.md#L75) — forward reference breaking rule 9: TSS is mentioned with only a passing gloss. Either expand the inline definition to "a CPU data structure that holds the kernel stack pointer the CPU must load when user code requests a privilege transition," or move the sentence to the end-of-chapter summary.

### ch03-kernel-entry
**Overall read**: The densest chapter so far — startup sequence, VGA, framebuffer desktop, and scrollback in quick succession. High risk of overwhelming readers.

- [ch03-kernel-entry.md:14-35](../ch03-kernel-entry.md#L14-L35) — paragraph overload: the Startup Sequence is one mega-paragraph stringing together nine distinct bootstrap steps (SSE, memory, paging, framebuffer, heap, VGA capture, GDT rebuild, IDT, device wiring, interrupts, mount, shell). Each deserves its own short paragraph.
- [ch03-kernel-entry.md:15](../ch03-kernel-entry.md#L15) — undefined acronym: **SIMD** and **SSE** appear mid-sentence with definitions tucked behind. Define both *one sentence before* they first matter.
- [ch03-kernel-entry.md:41-43](../ch03-kernel-entry.md#L41-L43) — missing "why": VGA text buffer is described but not justified. Add: "The VGA text buffer is the one display mechanism available immediately after boot, before any graphics drivers exist. It's simple enough to use from assembly and is our first output path."
- [ch03-kernel-entry.md:103-113](../ch03-kernel-entry.md#L103-L113) — jargon stacking: window table, z-order, hit-testing, back-buffer, dirty rectangles, and PS/2 mouse appear in two paragraphs with no glosses. Break the framebuffer desktop discussion into a separate subsection with term introductions on first use.

### ch04-interrupts
**Overall read**: Methodical and well-structured. Moves cleanly from concept → exception vectors → hardware IRQs → PIC remapping → asm stubs → C frames.

- [ch04-interrupts.md:9](../ch04-interrupts.md#L9) — **keep as-is** exemplar: the definition of *interrupt* is ideal rule-2 execution.
- [ch04-interrupts.md:71-88](../ch04-interrupts.md#L71-L88) — missing everyday analogy: the PIC remap section is correct but abstract. Suggest: "This collision is like a telephone exchange where extension 8 is wired to both the fire department and the mail room — when extension 8 rings, nobody knows which one is calling."
- [ch04-interrupts.md:127-137](../ch04-interrupts.md#L127-L137) — missing "why": the stubs explanation needs one more sentence — "The CPU's interrupt-entry frame layout is fixed by the hardware spec and cannot be changed to match C conventions; the stubs are the only place we can translate between the two."

### ch05-irq-dispatch
**Overall read**: Cleanly structured and focused. Solves a real coupling problem with a simple table lookup.

- [ch05-irq-dispatch.md:6-11](../ch05-irq-dispatch.md#L6-L11) — **keep as-is** exemplar: the "why a dispatcher" motivation is ideal.
- [ch05-irq-dispatch.md:75-85](../ch05-irq-dispatch.md#L75-L85) — visual opportunity: the RTC stable-read loop is described as one long sentence with three outcomes. A small decision-tree diagram would communicate faster.

### ch06-sse
**Overall read**: Focused and clear. Problem → control bits → template approach → context-switch integration, with good pacing.

- [ch06-sse.md:9-10](../ch06-sse.md#L9-L10) — missing everyday analogy: suggest "It's like a multitasking office where employees share one big desk. If the desk has hidden drawers that only some employees know about, and one forgets to close a drawer, the next employee may overwrite what's inside. So the OS keeps the drawers locked until the scheduler promises to manage them."
- [ch06-sse.md:63-64](../ch06-sse.md#L63-L64) — missing "why": the 16-byte alignment requirement for FXSAVE is stated but not motivated. Add a brief line on the hardware-compatibility reason.
- [ch06-sse.md:80-89](../ch06-sse.md#L80-L89) — weak chapter ending: the "Where the Machine Is" section reads as a feature list. Expand into the narrative form rule 6 expects — what the reader should now hold in their head and what comes next.

---

## Part II — Memory

### ch07-memory-detection
**Overall read**: Accessible and well-scaffolded, but front-loads architectural context before the timing problem.

- [ch07-memory-detection.md:5-11](../ch07-memory-detection.md#L5-L11) — cold-start: opens by listing "two things the kernel needs" but doesn't explain *why now*. Suggest opening with "Before we hand out any memory, the kernel must discover which regions of the physical address space are actually RAM and which are reserved for hardware. On a real PC, this is not obvious — and we must find out before we leave real mode."
- [ch07-memory-detection.md:23-34](../ch07-memory-detection.md#L23-L34) — undefined term: **ACPI** is used mid-paragraph, then back-explained. Introduce on first use.
- [ch07-memory-detection.md:48-72](../ch07-memory-detection.md#L48-L72) — paragraph overload + missing "why": 24 lines covering conservative-init, type-1 filtering, and the +4 trap. Break into two paragraphs and move the self-describing-format rationale ahead of the trap.
- [ch07-memory-detection.md:74-91](../ch07-memory-detection.md#L74-L91) — weak chapter ending: restates the fact of a reliable map without orienting toward Chapter 8.

### ch08-memory-management
**Overall read**: Comprehensive and well-illustrated; the three-layer framing is clear but front-loads PMM bitmap details before the problem each layer solves.

- [ch08-memory-management.md:5-16](../ch08-memory-management.md#L5-L16) — cold-start: chapter names three subsystems before explaining why each exists. Open with one concrete problem per layer (track free pages; translate virtual→physical; allocate small kernel objects without wasting whole pages).
- [ch08-memory-management.md:42-75](../ch08-memory-management.md#L42-L75) — missing everyday analogy for the bitmap: "think of the bitmap like a seating chart at a concert — each seat is one page, a `1` means someone is sitting there, a `0` is empty. To find a seat, scan for the first empty row, then the first empty chair in that row."
- [ch08-memory-management.md:129-141](../ch08-memory-management.md#L129-L141) — jargon stacking: PDE, virtual address, identity mapping, page directory, page tables stacked together. Lead with the *problem* ("if processes shared a page table, writes from one would overwrite another").
- [ch08-memory-management.md:155-191](../ch08-memory-management.md#L155-L191) — paragraph overload: heap section mixes block-header structure, splitting, coalescing, and magic canaries. Move the canary into a short aside after alloc/free is established.
- [ch08-memory-management.md:266-278](../ch08-memory-management.md#L266-L278) — weak chapter ending: summary table is good but add one narrative sentence bridging to Chapter 9.

---

## Part III — Hardware Interfaces

### ch09-klog
**Overall read**: Tightly written and clear — purpose obvious, implementation simple. The biggest issues are a thin on-ramp and a perfunctory ending.

- [ch09-klog.md:5-10](../ch09-klog.md#L5-L10) — cold-start: "every subsystem needs to report status" is abstract. Add one concrete sentence: "Without a shared log, each subsystem invents its own messaging, the output becomes noisy, and once the screen scrolls the boot history is gone."
- [ch09-klog.md:11-27](../ch09-klog.md#L11-L27) — missing analogy: "the ring buffer is like a revolving door with a fixed number of slots — new messages push out old ones as the head pointer wraps, but the consumer reads at its own pace from the tail."
- [ch09-klog.md:39-42](../ch09-klog.md#L39-L42) — weak chapter ending: four-line section. Expand to orient: what user tooling (`dmesg`, `/proc/kmsg`) this eventually makes possible.

### ch10-keyboard
**Overall read**: Well-structured hardware-to-software flow. One serious jargon stack; modifier keys and signal delivery are tangled.

- [ch10-keyboard.md:32-56](../ch10-keyboard.md#L32-L56) — jargon + analogy: the ring-buffer head/tail equality rule reads twice. Simplify: "When the head catches up to the tail, the buffer is full and the next keystroke is dropped. When they're equal, the buffer is empty."
- [ch10-keyboard.md:57-71](../ch10-keyboard.md#L57-L71) — paragraph overload: Shift tracking, Ctrl+C signal delivery, and Ctrl+Z suspend are mixed. Split: one paragraph for modifier tracking, one for signal-delivery policy.
- [ch10-keyboard.md:88-99](../ch10-keyboard.md#L88-L99) — undefined term: extended scancodes appear with a 0xE0-prefix table but no lead-in. Add: "Extended scancodes are two-byte sequences: a 0xE0 prefix followed by a key-identifying byte."
- [ch10-keyboard.md:101-110](../ch10-keyboard.md#L101-L110) — weak chapter ending: strong pattern-abstraction but doesn't state what the reader can now *do* (a user program can `SYS_READ` from the ring buffer).

### ch11-ata-disk
**Overall read**: Well-organized protocol explanation. Minor on-ramp and jargon issues.

- [ch11-ata-disk.md:11-19](../ch11-ata-disk.md#L11-L19) — missing analogy: "PIO is like the kernel carrying each word from disk to RAM by hand, one at a time — slow but simple. DMA is like setting up an automatic conveyor belt that moves data while the kernel does other work."
- [ch11-ata-disk.md:21-24](../ch11-ata-disk.md#L21-L24) — cold-start: opens with "Two Ways to Move Data" before establishing why we're talking to disks now. One sentence bridging from memory chapters would help.
- [ch11-ata-disk.md:59-84](../ch11-ata-disk.md#L59-L84) — paragraph overload: LBA28 section mixes bit-field layout, CHS history, register packing, and fixed bits. Tighten CHS to one sentence and split the bit layout into its own subsection.
- [ch11-ata-disk.md:122-125](../ch11-ata-disk.md#L122-L125) — weak chapter ending: add "by end of chapter, the kernel can read and write arbitrary 512-byte sectors without any filesystem knowledge."

### ch12-device-registries
**Overall read**: Clear design motivation. One serious jargon stack; registry concept itself lacks an analogy.

- [ch12-device-registries.md:5-10](../ch12-device-registries.md#L5-L10) — cold-start + analogy: "Think of the device registry like a phone directory: drivers publish their contact info under a short name, consumers look up the name when they need to reach a device. No compile-time dependency — just a runtime lookup."
- [ch12-device-registries.md:42-46](../ch12-device-registries.md#L42-L46) — undefined terms: "whole disk" vs. "partition" distinction isn't introduced until two paragraphs later. Define on first use.
- [ch12-device-registries.md:59-72](../ch12-device-registries.md#L59-L72) — paragraph overload: MBR discovery crams partition-record description, MBR magic bytes, and name formation into 14 lines. Split into three short paragraphs.
- [ch12-device-registries.md:117-120](../ch12-device-registries.md#L117-L120) — weak chapter ending: simplify the closing to a crisp one-liner on what the pattern now enables.

---

## Part IV — Files and Storage

### ch13-filesystem
**Overall read**: Clear and thorough, but dense in the middle. Inode motivation would land better with an analogy.

- [ch13-filesystem.md:13-22](../ch13-filesystem.md#L13-L22) — cold-start: the four "why inodes" reasons are listed too quickly without concrete anchoring. Flesh out "directory entries bloat with metadata" into "when a name lookup reads a directory entry, it fetches timestamps and sizes it doesn't need — wasting disk reads."
- [ch13-filesystem.md:106-112](../ch13-filesystem.md#L106-L112) — jargon + missing "why": "inodes are 128 bytes with four per sector" assumes familiarity with the ratio. Unpack: multiple inodes fit in one 512-byte ATA sector, so a single sector read yields four inodes.
- [ch13-filesystem.md:134-139](../ch13-filesystem.md#L134-L139) — paragraph overload: compresses on-demand allocation, bitmap flush strategy, and the optimization into one block. Split.
- [ch13-filesystem.md:167-172](../ch13-filesystem.md#L167-L172) — missing "why": breadth-first inode numbering helps cache locality. State the reason.

### ch14-vfs
**Overall read**: Concise and well-structured; ops-table and registration flow logically. Ending doesn't bridge to Chapter 15.

- [ch14-vfs.md:5-13](../ch14-vfs.md#L5-L13) — cold-start: the abstraction's value is implicit. Make it concrete: "Without a VFS layer, the ELF loader and every syscall handler would contain hardcoded DUFS or ext3 logic. Adding a third filesystem would mean updating code in ten places."
- [ch14-vfs.md:76](../ch14-vfs.md#L76) — undefined term: **synthetic** filesystems (devfs, procfs, sysfs) used without definition. Add: "synthetic means the VFS generates the contents rather than reading from a disk backend."
- [ch14-vfs.md:174-176](../ch14-vfs.md#L174-L176) — missing "why": richer node types exist because the syscall layer needs to dispatch between filesystem ops and synthetic handlers. State it.
- [ch14-vfs.md:191-193](../ch14-vfs.md#L191-L193) — weak chapter ending: add a bridge to Chapter 15 — the VFS is now the single interface, and next we build the processes that will use it.

---

## Part V — Running User Programs

### ch15-processes
**Overall read**: Comprehensive and technically sound, but the first three sections form a dense cold start.

- [ch15-processes.md:15-29](../ch15-processes.md#L15-L29) — cold-start + jargon stacking: four hardware structures (GDT, TSS, IDT, paging) listed without narrative. Open with the *problem* ("ring 3 code cannot touch kernel memory, but the CPU must still switch to a kernel stack on interrupt") and then enumerate the structures.
- [ch15-processes.md:29](../ch15-processes.md#L29) — undefined term: **DPL 0** appears in a table cold. Add: "Descriptor Privilege Level — DPL 0 means only ring 0, DPL 3 means ring 3 or lower."
- [ch15-processes.md:49-54](../ch15-processes.md#L49-L54) — missing "why": per-process page directories need the PG_USER-bit rationale spelled out.
- [ch15-processes.md:108-145](../ch15-processes.md#L108-L145) — paragraph overload: process descriptor struct and state machine back-to-back with no transition. Add "several new fields manage preemption and blocking:" and "a process moves through states as events fire:" sentences.
- [ch15-processes.md:150-153](../ch15-processes.md#L150-L153) — jargon stacking: PIT + divisor + Intel 8253/8254 arrive too fast. Simplify: "The PIT is an old timer chip. The kernel tells it to fire an interrupt every ten milliseconds."

### ch16-syscalls
**Overall read**: Well-paced. The path-of-a-syscall diagram and blocking mechanics are clear. Ending is weak.

- [ch16-syscalls.md:19-20](../ch16-syscalls.md#L19-L20) — undefined term: **signal** defined mid-paragraph rather than on first use.
- [ch16-syscalls.md:103-105](../ch16-syscalls.md#L103-L105) — weak chapter ending: the section lists mechanics but doesn't bridge to Chapter 17. Add: "Every syscall identifies files by inode number. The next chapter introduces the file descriptor table that sits between the syscall layer and those inodes."

### ch17-file-io
**Overall read**: Tight narrative flow. The opening uses a confusing negation.

- [ch17-file-io.md:5-9](../ch17-file-io.md#L5-L9) — cold-start: "The fd is not a pointer into the filesystem — it is an index..." leads with a negation. Reframe: "Every open file is identified by a small integer called a **file descriptor** (fd), which is an index into a per-process table. This is what lets a process track multiple open files with simple numbers like 0, 1, 2."
- [ch17-file-io.md:31-35](../ch17-file-io.md#L31-L35) — jargon stacking: inode, offset, size, EOF in one sentence. Split into two: "the kernel checks whether the read position has reached the file size. If so, it returns 0, which signals **EOF** (End of File)."

### ch18-tty
**Overall read**: Engaging and well-motivated. Signal-generation rationale is buried.

- [ch18-tty.md:11-17](../ch18-tty.md#L11-L17) — missing "why": lift the signal-generation rationale to the top: "The TTY — not the keyboard driver — generates signals because the TTY knows which process group is in the foreground. The keyboard has no concept of process groups."
- [ch18-tty.md:43-44](../ch18-tty.md#L43-L44) — undefined term: `VMIN=1`, `VTIME=0` appear without definition.
- [ch18-tty.md:119-121](../ch18-tty.md#L119-L121) — weak chapter ending: doesn't bridge to the next topic.

---

## Part VI — User Environment

### ch19-signals
**Overall read**: Strong chapter with clear mechanics; jargon density spikes mid-chapter.

- [ch19-signals.md:24-34](../ch19-signals.md#L24-L34) — undefined terms: **NSIG**, **TGID**, **TID** used without plain-English glosses on first use.
- [ch19-signals.md:42-59](../ch19-signals.md#L42-L59) — paragraph overload: SIGCONT, SIGSTOP/SIGTSTP, SIGKILL special cases compressed into one block. Break each into its own short paragraph.
- [ch19-signals.md:61-72](../ch19-signals.md#L61-L72) — cold-start for the Delivery Window section: open with the danger, not the mechanism. "The kernel faces a dangerous moment: a signal can arrive while the process is mid-instruction. We solve this by deferring delivery until one specific safe point — right before returning to user space."
- [ch19-signals.md:73-81](../ch19-signals.md#L73-L81) — missing "why": explain why task-directed signals are checked before process-directed ones.

### ch20-user-runtime
**Overall read**: Clear progression from concept to implementation; well-scaffolded.

- [ch20-user-runtime.md:47-54](../ch20-user-runtime.md#L47-L54) — missing analogy: "Think of the heap like a table at a restaurant. You start with a small surface. As you need more room you push the edge back (`brk`) to claim more real estate. The page-fault handler is the kitchen staff who only lay down plates (physical pages) when you actually seat a guest there."
- [ch20-user-runtime.md:56-65](../ch20-user-runtime.md#L56-L65) — cold-start for the free-list allocator: jumps into implementation without stating the goal. Open with "The heap needs to carve memory into blocks and reclaim them. The strategy: each allocation carries a header with its size and status; free blocks are chained so malloc can scan for a fit."
- [ch20-user-runtime.md:33](../ch20-user-runtime.md#L33) — undefined term: "output constraint" in the GCC inline-asm discussion assumes familiarity with extended-asm syntax.

### ch21-libc
**Overall read**: Excellent structure; the layering diagram is well-placed; voice stays conversational.

- [ch21-libc.md:25-29](../ch21-libc.md#L25-L29) — missing "why" for `memmove`: add "If we copied forward, the destination pointer would overwrite source bytes before we read them. Copying backward ensures every source byte is consumed before the destination catches up."
- [ch21-libc.md:31-46](../ch21-libc.md#L31-L46) — missing analogy for the sink abstraction: "Imagine a mail sorter who doesn't care whether they're sorting into a bin (stream) or a postbox (buffer). The sink lets one format engine serve both — it calls a callback for each character and the callback decides where it goes."
- [ch21-libc.md:74-80](../ch21-libc.md#L74-L80) — cold-start on calendar time: the leap-year arithmetic section is dense. Open with "the kernel hands us a counter; libc's job is to turn that into human-readable dates."

### ch22-shell
**Overall read**: Long and detailed; strong operational clarity but a few sections pile up mechanism without pausing.

- [ch22-shell.md:35-42](../ch22-shell.md#L35-L42) — cold-start on PATH resolution: precede with a one-sentence mental model: "The shell needs to find where a command lives on disk. If the user typed a path, trust it; otherwise search each directory in PATH until a match appears."
- [ch22-shell.md:44-56](../ch22-shell.md#L44-L56) — missing "why" for each step of external-program launch: in particular, why `SYS_SETPGID` puts the child in its own process group. Answer: so the TTY can direct keyboard signals to the whole group.
- [ch22-shell.md:57-68](../ch22-shell.md#L57-L68) — jargon stacking in the pipe section: five concepts (pipe creation, two forks, dup2, EOF semantics, parent close). Add: "EOF only happens when *every* copy of the write end is closed; by closing our copy in the parent, we ensure the child sees EOF when the left program exits."
- [ch22-shell.md:103-108](../ch22-shell.md#L103-L108) — paragraph overload: "Running Inside the Desktop" mixes window architecture, focus, input routing, the taskbar, and shell decoupling. Split into three short paragraphs.

---

## Part VII — Extending the Kernel

### ch23-modules
**Overall read**: Cleanly structured with a good visual; jargon-light and well-scaffolded.

- [ch23-modules.md:20-23](../ch23-modules.md#L20-L23) — cold-start: allocatable vs. non-allocatable sections are used before the `SHF_ALLOC` flag concept is introduced.
- [ch23-modules.md:26-32](../ch23-modules.md#L26-L32) — missing analogy for relocation: "Think of relocation like sorting a package labelled 'recipient unknown'. The compiler left a placeholder. When we load the module, we look up the real address and fill it in — exactly as the mail carrier would look up and write the recipient's address."
- [ch23-modules.md:27-32](../ch23-modules.md#L27-L32) — undefined terms: `R_386_32` and `R_386_PC32` need one-sentence glosses ("absolute 32-bit address" vs. "PC-relative offset, used by `call`").

### ch24-core-dumps
**Overall read**: Strong technical chapter; a few places assume fresh ELF familiarity that earlier chapters introduced but this chapter doesn't re-cue.

- [ch24-core-dumps.md:12-16](../ch24-core-dumps.md#L12-L16) — missing "why": why capture the crash frame immediately? "The process's memory may be corrupted afterward; if we tried to reconstruct the register state later the stack might be overwritten. Copying the frame in the exception handler preserves it before any cleanup."
- [ch24-core-dumps.md:17-26](../ch24-core-dumps.md#L17-L26) — cold-start on the ET_CORE format: bridging sentence needed — "an ELF file contains a header describing the file, followed by program headers that describe how to load segments. A core file uses the same structure but with special segment types."
- [ch24-core-dumps.md:124-125](../ch24-core-dumps.md#L124-L125) — weak chapter ending: expand to describe the complete forensic record a crash now yields, and why GDB can load it.

---

## Part VIII — Fault-Driven Memory

### ch25-demand-paging
**Overall read**: Strong opening arc; occasional jargon-stacking and one weak diagram integration point.

- [ch25-demand-paging.md:5-11](../ch25-demand-paging.md#L5-L11) — cold-start: narratively strong but jumps to `brk()` without reestablishing the heap mental model. Add a one-sentence anchor: "Recall from Chapter 20 that the process heap is a range of virtual addresses the program can request from the kernel."
- [ch25-demand-paging.md:42-58](../ch25-demand-paging.md#L42-L58) — missing "why" for the 6-step recovery path: "This works because the CPU stopped at a well-defined point — CR2 holds the faulting address and the instruction that caused the fault is known, so we can simply resume it once the page is present."
- [ch25-demand-paging.md:69](../ch25-demand-paging.md#L69) — jargon stacking: "mapping step itself invalidates the stale TLB..." piles three concepts. Unpack TLB inline.
- [ch25-demand-paging.md:85-91](../ch25-demand-paging.md#L85-L91) — undefined term: **VMA** used before its first spell-out.

### ch26-copy-on-write-fork
**Overall read**: Technically clear with strong before/after tables; two-pass strategy lacks motivation.

- [ch26-copy-on-write-fork.md:13-30](../ch26-copy-on-write-fork.md#L13-L30) — missing analogy for refcounts: "Think of it like a library book with multiple holds. The book stays out as long as at least one patron still has a claim on it. Only when the last hold is released does it go back on the shelf."
- [ch26-copy-on-write-fork.md:36-59](../ch26-copy-on-write-fork.md#L36-L59) — paragraph overload: algorithm outline, PDE check, kernel-entry preservation, the result, and the rationale are all in one block. Split into three paragraphs.
- [ch26-copy-on-write-fork.md:109-117](../ch26-copy-on-write-fork.md#L109-L117) — weak chapter ending: bridge to Chapter 27: "With fork now deferred and memory shared until write-time, the kernel is ready to wire up the remaining devices — starting with the mouse, which will bring the desktop to life."

---

## Part IX — Graphical Environment

### ch27-mouse
**Overall read**: Excellent cold-start and narrative structure. Exemplary opening sequence.

- [ch27-mouse.md:1-19](../ch27-mouse.md#L1-L19) — **keep as-is** exemplar: the seven-step opener is a model for how to ground the reader before register-level detail.
- [ch27-mouse.md:22-31](../ch27-mouse.md#L22-L31) — undefined term: **PS/2 auxiliary channel** needs a plain-English gloss — "the second input channel of the 8042 keyboard controller, physically separate but sharing the same I/O ports."
- [ch27-mouse.md:45-54](../ch27-mouse.md#L45-L54) — jargon stacking in the packet-structure table: separate packet fields from synchronisation flags in different sentences.
- [ch27-mouse.md:82-87](../ch27-mouse.md#L82-L87) — missing "why" for coalescing: "dropping intermediate motion packets trades occasional pixel-jitter for lower latency, which feels more responsive than drawing every packet."

### ch28-desktop
**Overall read**: Clear architecture with good visual breaks; one dense opening.

- [ch28-desktop.md:5-15](../ch28-desktop.md#L5-L15) — cold-start: opens with two display modes before the reader knows what a compositor does. Reorder: compositor role first, display modes second.
- [ch28-desktop.md:78-84](../ch28-desktop.md#L78-L84) — missing "why" for z-order and focus: "Z-order ensures the most recent window appears on top; focus routing ensures your keystrokes go to the window you're looking at, not a window hidden behind."
- [ch28-desktop.md:119-124](../ch28-desktop.md#L119-L124) — missing "why" for the `cli`/`sti` critical section: "Only the compositor can modify window state and the framebuffer, so we protect just that operation rather than adding locks everywhere an IRQ might land."

---

## Part X — Development Tools

### ch29-debugging
**Overall read**: Well-structured with clear phase separation; one forward-reference issue.

- [ch29-debugging.md:11-42](../ch29-debugging.md#L11-L42) — **keep as-is** exemplar: the three-phase model ("GDB attaches → breakpoint hits → type next") is exceptional. Retain.
- [ch29-debugging.md:73-79](../ch29-debugging.md#L73-L79) — missing "why" in "Logs Still Matter": logs survive kernel crashes because serial and QEMU debug-port writes bypass the console path. State this.
- [ch29-debugging.md:94-99](../ch29-debugging.md#L94-L99) — undefined forward reference: `/proc/<pid>/vmstat` and `/proc/<pid>/fault` cited without a quick reminder that these were introduced in Chapter 24.

### ch30-cpp-userland
**Overall read**: Excellent clarity and scaffolding; one acronym used before definition.

- [ch30-cpp-userland.md:5-11](../ch30-cpp-userland.md#L5-L11) — undefined term: **RTTI** used in the first paragraph without gloss. Add "(Run-Time Type Information, metadata the compiler stores so `dynamic_cast` and `typeid` can work at runtime)."
- [ch30-cpp-userland.md:34-51](../ch30-cpp-userland.md#L34-L51) — cold-start: opens by mentioning `_start` without reminding the reader this came from Chapter 20.
- [ch30-cpp-userland.md:73-79](../ch30-cpp-userland.md#L73-L79) — missing "why" on ABI boundary: "keeping the boundary sharp prevents a program from accidentally depending on libraries we can't run in a freestanding environment."

### ch31-aarch64-bringup
**Overall read**: Excellent scope discipline and "narrow waist" framing. Strongest chapter-opening philosophy in the book.

- [ch31-aarch64-bringup.md:5-22](../ch31-aarch64-bringup.md#L5-L22) — **keep as-is** exemplar: the "Why The First Slice Is So Small" section is a model for grounding a reader in a design choice before diving into implementation.
- [ch31-aarch64-bringup.md:25-34](../ch31-aarch64-bringup.md#L25-L34) — undefined terms: **EL2** and **EL1** need glosses — "Exception Level 2, a privileged mode for hypervisors" and "Exception Level 1, the standard privileged mode for ordinary operating systems."
- [ch31-aarch64-bringup.md:38-52](../ch31-aarch64-bringup.md#L38-L52) — cold-start in "Exception Vectors Replace The IDT": compare to x86 IDT but re-anchor it — "just as the x86 IDT (Chapter 4) tells the CPU where to jump on an exception, AArch64 uses a vector table."
- [ch31-aarch64-bringup.md:70-77](../ch31-aarch64-bringup.md#L70-L77) — missing "why" on timer interval: "one interrupt per second — slow enough to see distinct ticks on the console, fast enough to prove the interrupt path works reliably."

---

## Part Introductions

### cover.md
Not reviewed separately — no issues identified.

### partI-firmware-to-kernel
**Overall read**: Excellent narrative grounding — "the machine is alive but barely governable" is gold-standard on-ramp language.

- [partI-firmware-to-kernel.md:1-3](../partI-firmware-to-kernel.md#L1-L3) — **keep as-is** exemplar of the part-intro form.
- Minor: "Programmable Interval Timer" is referenced at line 7 without unpacking; acceptable given the preceding interrupt-architecture framing.

### partII-memory
**Overall read**: Clear problem-layering; three nested problems is a strong framing.

- [partII-memory.md:3](../partII-memory.md#L3) — **E820** and **BIOS** are used cold in the opening paragraph. Suggest "The BIOS (Basic Input/Output System, the firmware that ran before GRUB) provides a memory map..."

### partIII-hardware-interfaces
**Overall read**: Strong "two dimensions" framework but jargon-heavy on hardware terms.

- [partIII-hardware-interfaces.md:3](../partIII-hardware-interfaces.md#L3) — **keep as-is** exemplar: "keys and hands" metaphor is a strong unifier.
- [partIII-hardware-interfaces.md:5](../partIII-hardware-interfaces.md#L5) — PS/2, scancodes, LBA all named before definition. Reorder: "the keyboard driver responds to PS/2 (the older round-connector interface), which sends numeric codes (scancodes) for each key press."

### partIV-files-and-storage
**Overall read**: Accessible and well-motivated — "sector number is not a name" is concrete and clear.

- [partIV-files-and-storage.md:1-3](../partIV-files-and-storage.md#L1-L3) — **keep as-is** exemplar opening.
- [partIV-files-and-storage.md:5](../partIV-files-and-storage.md#L5) — "inode-based layout" leads with the technical name before the concept. Reorder: "each file's metadata lives in a fixed-size record (an inode)".

### partV-running-user-programs
**Overall read**: Dense but authoritative. Privilege boundary is clearly established.

- [partV-running-user-programs.md:5](../partV-running-user-programs.md#L5) — the pipeline paragraph is a single 8-line sentence describing six steps. Break into 2-3 sentences.

### partVI-user-environment
**Overall read**: Nearly ideal. Strong framing, careful term introduction, no condescension.

- No findings — use this part intro as the reference template for the others.
- Minor inconsistency: intro says "four distinct problems" but lists five.

### partVII-extending-the-kernel
**Overall read**: Clear problem statement; ties two problems through a common thread (ELF understanding).

- [partVII-extending-the-kernel.md:3](../partVII-extending-the-kernel.md#L3) — sharpen "there is no record" to "crashed processes disappear, and the kernel has no way to record what went wrong."

### partVIII-fault-driven-memory
**Overall read**: Good problem exposition; the closing metaphor is strong.

- [partVIII-fault-driven-memory.md:3](../partVIII-fault-driven-memory.md#L3) — **identity map** used cold. Add: "a fixed identity map (where virtual addresses equal physical addresses)."

### partIX-graphical-environment
**Overall read**: Accessible; motivation is clear.

- [partIX-graphical-environment.md:5](../partIX-graphical-environment.md#L5) — **procfs** named without definition. Add one clause.

### partX-development-tools
**Overall read**: **Weakest part intro in the set.** Too sparse; does not orient the reader on why these two topics belong together.

- [partX-development-tools.md:1-3](../partX-development-tools.md#L1-L3) — abstract opener "the bar for development work" needs grounding. Suggest: "Once processes can crash, the kernel needs a way to capture and analyze the crash state. Once userland can run C programs, developers want to write C++ too. Part X covers both."
- [partX-development-tools.md:5](../partX-development-tools.md#L5) — the link between kernel debugging and C++ support is not explained. Either articulate the shared thread or name this part as a collection of developer conveniences rather than a unified subsystem.
- [partX-development-tools.md:5](../partX-development-tools.md#L5) — **ABI** named without unpacking. Given the part is already spare, define it.

---

## Cross-cutting patterns

1. **Cold-starts where the reader doesn't know *why now*.** Several chapters open by naming the topic without restating the problem the prior chapter left open. The strongest openings (ch04, ch27, ch31, partI, partIV) open with the problem; the weakest (ch03, ch07, ch08 opening, ch13, ch15 opening) open with the solution.

2. **Missing everyday analogies at the conceptual core.** Page faults, reference counting, ring buffers, the sink abstraction, refcounts, relocation, `memmove`, the device registry, demand paging — all of these would benefit from a single sentence with a concrete analogy before the technical explanation. The book already does this well in places (ch04 timer exchange, ch06 desk drawers in the SSE suggestion, ch27 seven-step mouse walk) but it's inconsistent.

3. **Undefined acronyms and terms on first use.** NSIG, TGID, TID, RTTI, VMA, EL1/EL2, DPL, E820, PS/2, LBA, ABI, synthetic filesystem, identity map, SIMD, SSE, VMIN/VTIME, R_386_32, ACPI, BIOS — collectively a large rule-2/rule-9 debt scattered across the book. A mechanical pass on "every acronym gets a one-sentence gloss on first use in a chapter" would pay for itself.

4. **Jargon stacking.** Chapters 2, 3, 8, 15, 17, 19, 22 have sentences where three or more OS terms pile up. Splitting each into two or three shorter sentences almost always reads better.

5. **Paragraph overload.** Chapters 3, 7, 8, 10, 15, 19, 22, 26 have mega-paragraphs that deserve to be broken into 2-5 smaller ones. Often the narrative arc is correct but the visual air is missing.

6. **Missing "why".** A recurring pattern — the *what* is explained but the design rationale is absent. The strongest chapters (ch04, ch05, ch29, ch31) explicitly justify choices; the weaker moments leave the reader with correct mechanics but no intuition about tradeoffs.

7. **Weak chapter endings.** Chapters 6, 7, 9, 10, 11, 12, 14, 16, 18, 24, 26 have "Where the Machine Is" sections that are either minimal, feature-list style, or don't bridge to the next chapter. [docs/style.md](../style.md) rule 6 wants a narrative orientation — many of these would grow by one or two sentences.

## Reference templates

When editing for ELI5 spirit, imitate these passages that already get it right:

- **ch01 opening** — framing firmware as "a tiny program baked into the motherboard" (proposed addition).
- **ch04 definition of *interrupt*** (line 9) — rule-2 execution.
- **ch05 motivation for the IRQ dispatcher** (lines 6-11) — explains *why* before *what*.
- **ch27 seven-step mouse walk** (lines 1-19) — grounds the reader before any register-level detail.
- **ch29 three-phase debugging model** (lines 11-42) — cleanly separates independent concerns.
- **ch31 "Why The First Slice Is So Small"** (lines 5-22) — best scope-setting in the book.
- **partI-firmware-to-kernel** — best part-intro in the book.
- **partVI-user-environment** — reference template for part intros.

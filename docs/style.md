# Documentation Style

This book is written for software engineers who have never built an operating system. Every chapter must follow these rules.

1. **Narrate the runtime flow.** Describe what is happening inside the machine at the moment the chapter covers — what the CPU is doing, what state is already established, what comes next. Avoid phrasing chapters as catalogues of features ("this chapter adds X"). Prefer "by the time the CPU reaches this point, X is true".

2. **Introduce all terms on first use.** Every acronym and OS-specific term must be spelled out and given a one-sentence plain-English definition the first time it appears in a chapter. If a term recurs heavily across chapters, re-introduce it briefly so the chapter is largely self-contained. This rule covers *terminology*; rule 9 covers the broader requirement to never use any *concept* before it has been explained. When a term also appears as a diagram label, use the same underlying words in prose (see rule 4 for diagram label capitalisation conventions).

3. **Use minimal code blocks.** Prefer prose descriptions of what the code does over pasting the code itself. The only code that belongs in a code block is:
   - **C struct definitions** — these are declarative and reading them is faster than describing them.
   - **Short tables** of register layouts, I/O port maps, access-byte bit fields, or similar data-dense material.
   - A brief inline snippet where the exact syntax is the point being explained.

   Do not paste inline assembly, C functions, or NASM macros verbatim when a paragraph of prose conveys the same information.

4. **Use visual aids generously for complex concepts.** Whenever a concept has a spatial, structural, or hierarchical shape, show it — do not only describe it in prose. Readers understand layouts, address ranges, bit fields, and multi-step lookups much faster when they can *see* the shape on the page. A good rule of thumb: if you catch yourself writing "the first N bytes hold X, the next M bytes hold Y, and after that comes Z", stop and draw it instead.

   Concepts that should almost always be accompanied by a diagram:
   - **Memory layouts** — the physical address map, where the kernel image sits, where the stack grows, where the PMM bitmap lives, where the kernel heap begins and ends. Show address ranges as a stacked box diagram with addresses on the left, or as a table (`start | end | region | notes`).
   - **Allocations and free lists** — how a region is carved up, which blocks are in-use vs free, how a freed block rejoins the pool. A before/after pair of boxes is usually clearer than three paragraphs of prose.
   - **Paging and address translation** — the page directory → page table → frame walk, the split of a linear address into directory index, table index, and offset, the bit layout of a PDE or PTE. Draw the 32-bit linear address as a bit-field bar, and draw the walk as arrows between boxes.
   - **Descriptor tables** — the GDT/IDT/TSS byte layouts, the access byte and flags nibble, segment selectors and their index/TI/RPL fields.
   - **On-disk structures** — the filesystem superblock, inode tables, block bitmap, data region, and how an MBR or boot sector is laid out.
   - **Stack frames and context switches** — what the stack looks like on interrupt entry, what the `iret` frame contains, what a saved register context looks like.
   - **Process and address-space layouts** — the user address space (code, data, heap, stack), the kernel's view of it, and how the two are stitched together through page tables.

   Err on the side of *more* visuals: if a chapter has five paragraphs describing where things live in memory and no diagram, it is under-illustrated.

   **Diagram admission test.** Before adding a new diagram, or deciding to keep an existing one, ask:
   - **What single reader question does this diagram answer?** A diagram should have one clear job: "what lives where?", "what branches where?", "what gets copied?", or "what changes state?" If the answer is vague, the diagram is probably vague too.
   - **Is the idea actually spatial, structural, branching, or comparative?** If the information is not easier to understand as shape, flow, containment, or before/after state, prose is usually the clearer medium.
   - **Does the diagram teach something the prose does not teach as quickly?** A diagram must not be prose rewritten as five boxes connected by arrows. If it merely restates the paragraph without giving the reader a faster mental model, redesign it or remove it.
   - **Can the reader tell what changes and what stays fixed?** In a mutation, clone, or transition diagram, make the stable parts visually stable and the changed parts visually prominent.
   - **Is the diagram using the right visual form for the concept?** Memory layouts should look like layouts; decision logic should look like a decision tree; state transitions should look like a state machine; comparisons should look like before/after or side-by-side structures. Avoid generic linear flows when the real point is data shape or branching policy.
   - **Can the diagram stand on its own for ten seconds?** A reader should be able to glance at it and understand the basic claim without re-reading three surrounding paragraphs first. The prose should deepen the diagram, not rescue it.

   **Use the simplest shape that matches the concept.** Every diagram should be assigned to one of these buckets before it is drawn:
   - **Layout** for memory maps, stack frames, file layouts, buffers, and fixed regions where "what lives where" is the point.
   - **Segmented strip** for one bounded linear region divided into consecutive spans, especially heaps, reserved ranges, and other cases where segment size and start/end boundaries are the point.
   - **Bit field / packed format** for register bytes, descriptor fields, selectors, and other packed binary layouts. In the book source, prefer native Markdown tables for these when a simple low-to-high field listing teaches the idea clearly.
   - **Table / matrix** for registries, fd tables, vector maps, decision tables, and other small fixed record sets where row/column comparison is the point. In the book source, these should normally be written as native Markdown tables in the chapter itself rather than generated as SVGs.
   - **Layer stack** for software layers that sit on top of one another, such as VFS/file-operation stacks or libc layering.
   - **Lookup / walk** for pointer chasing, path walks, address translation, and other multi-step indirection where one structure leads to the next.
   - **Cyclic buffer** for fixed-slot arrays with head/tail cursors or wraparound semantics, where the reader needs to see both the linear slots and the wrap path.
   - **Grid snapshot** for sampled fixed grids with row and address annotations, such as the VGA text buffer.
   - **State snapshot** for "what is true at this exact moment" views such as boot handoff registers or other fixed machine snapshots.
   - **Decision tree** for "if this bit is set, do X; otherwise do Y" policy.
   - **State machine** for lifecycle and scheduler transitions.
   - **Comparison** for before/after or side-by-side changes such as heap splitting, coalescing, and copy-on-write effects.
   - **Timeline / handoff path** only when order itself is the teaching point. If the diagram is just a straight line of steps, be sure the reader is learning the handoff shape, not just rereading the paragraph in boxes.
   - **Annotated example** for rendered examples or transcripts, such as kernel-log output, where the concrete sample is the teaching tool.
   - **Tagged transcript** for structured console or log samples where each line has a short tag and a message body, usually in one vertical reading order rather than multiple columns.
   - **Buffer transfer** for repeated source units flowing into one destination buffer or region.
   - **Lane walk** for a set of parallel lookup paths that each show a different case of the same indirection rule.
   - **Split overview** for two vertically stacked views of the same artifact, such as "overall layout" above "starter contents".

   If a diagram does not fit one of these buckets, add a new bucket here first and name the exception explicitly before adding a one-off renderer.

   **Maintenance rule.** Every generated diagram should be referenced by at least one chapter, and every referenced diagram should earn its place. Remove stale or orphaned diagrams rather than letting the generator output drift away from the prose.

   **Diagram format.** Do not use ASCII art in the markdown source. All diagrams must be stored as SVG files in `docs/diagrams/` and referenced in the chapter with a standard Markdown image link:

   ```markdown
   ![](diagrams/chNN-diagNN.svg)
   ```

   The source of truth for those SVGs is `docs/generate_diagrams.py`. Use that script to generate every diagram SVG so spacing, typography, panel sizes, and colours stay consistent across the book. If a diagram needs a new layout, update the generator and re-run it; do not hand-edit the generated SVG as the primary workflow. The epub and PDF build pipelines embed SVGs directly — do not generate PNG copies. Generated diagrams also include a footer with the chapter and figure number for reader reference; keep that footer generator-driven rather than editing it by hand per file.

   Do not use the SVG pipeline for plain tables. If the information is fundamentally row/column data, write it natively in Markdown in the chapter instead of adding a `chNN-diagNN.svg`. This includes bitfield and packed-format diagrams when they are really just field listings.

   For native bitfield and packed-format tables, list rows from the lowest bit upward, and write contiguous ranges in low-to-high form (`0-1`, `12-31`) rather than descending notation (`1-0`, `31-12`).

   **Generator layout rule.** The generator should place content from panel-relative layout primitives, not from per-diagram pixel coordinates. Define a main panel, then let each bucket renderer define explicit edge-spacing tokens for the gap from the outer container to the rendered content. Do not let those gaps emerge from overall SVG height, centering inside oversized content areas, or ad hoc per-diagram padding. Bucket-level spacing tokens are acceptable when they enforce a clearer invariant across that whole shape family; for example, stack diagrams should keep a fixed top and bottom gap from the outer container to the first and last stack elements so every stack reads as the same visual form, and the same principle should apply to every other reusable bucket. The same rule applies horizontally: content-sized buckets should shrink the SVG canvas to the panel they actually need instead of leaving a page-width image around a much narrower container. The subtitle block under the title must also use one shared gap to the top of the outer container across all bucket types; do not let some buckets pull the panel upward with hardcoded `panel_y` values while others leave more space. If a diagram genuinely needs bespoke placement, keep the exception narrowly scoped in the generator and explain why the standard row/column/tree/layout helpers were not enough.

   Keep diagram numbering contiguous and in first-appearance order within each chapter. If a diagram is removed or moved, renumber the later diagrams in that chapter so the references continue as `chNN-diag01`, `chNN-diag02`, `chNN-diag03`, with no gaps and in the same order the reader encounters them.

   When creating a diagram:
   1. Add or update the diagram definition in `docs/generate_diagrams.py`, using the `chNN-diagNN.svg` naming convention.
   2. Run `docs/generate_diagrams.py` to regenerate the SVG into `docs/diagrams/`.
   3. Verify the rendered output before calling the work complete. Do not stop at inspecting the SVG source text or assuming the generator output is fine. Render the diagram and visually check the actual result for clipped text, overlaps, arrows touching labels, off-canvas content, and any layout that is hard to read. When generating PNGs for validation, always write them to a fresh temporary directory or otherwise unique filenames so viewers and tooling cannot serve stale artifacts from an earlier render. Use `docs/render_diagram_check.sh` rather than repeatedly overwriting one fixed file in `/tmp`. If the rendered image is not clean, revise the generator and re-render until it is.
   4. Keep the output in the current visual style: clean panels, readable labels, consistent spacing, no overlaps between text, arrows, and boxes. Use colour only when it is teaching something — separating "before" from "after", "kernel" from "user", or "success" from "failure". If colour is not carrying meaning, let layout, labels, and arrows do the work instead.
   5. Keep label formatting consistent within and across diagrams. Labels and sub-labels follow **Chicago Manual of Style sentence case**: capitalise only the first word and any proper nouns or acronyms that are conventionally uppercased; all other words are lowercase (`'Boot info pointer'`, `'Initial ESP'`, `'Stack pointer undefined on entry'`). Keep the grammatical shape consistent from box to box: if one label is a noun phrase, all labels in that diagram must be noun phrases. Prose that describes what a diagram shows must use the same underlying words as the diagram labels — if a diagram labels a region `'Boot info pointer'`, the prose must call it *boot info pointer*, not coin a different name for the same concept.
   6. Add the image reference to the chapter at the point in the prose where the diagram is first discussed.

   If the visual is really just a table, skip this workflow and add a native Markdown table directly to the chapter instead.

   **Decision trees need extra space.** A decision tree is not a compact flowchart. If a diagram branches, enlarge the canvas, widen the branch spacing, and increase the connector clearance until every branch label sits in open whitespace rather than against a box edge or arrow. Vertical connectors must have a clearly visible shaft. If there is any tension between compactness and legibility, choose legibility.

   Existing diagrams were converted from ASCII art by `docs/extract-diagrams.py` using `svgbob`, then normalised into the generator-driven SVG set. Re-run that script only if you need to bulk-migrate old ASCII-art source; new diagrams go through `docs/generate_diagrams.py`.

5. **Order chapters in layers — each chapter builds on the last.** The book teaches a reader who reads front to back. A chapter may only assume knowledge that a preceding chapter has established. Hardware must be introduced before the software that drives it. Data structures must be explained before the algorithms that operate on them. Abstractions must be grounded in their concrete implementation before being used as building blocks for higher-level concepts. When assigning a chapter number, ask: "Can a reader who just finished the previous chapter follow this one without unexplained gaps?" If not, the chapter is misplaced or a prerequisite chapter is missing. This ordering must be reflected both in the `DOCS_SRC` list in the Makefile and in the prose — a chapter should open by explicitly connecting to the state established by the chapter before it.

6. **Voice and tone.** Be warm, direct, and conversational — like a knowledgeable colleague walking you through something they find genuinely interesting, not a reference manual. Write in the first-person plural ("we", "let's") to bring the reader along as an active participant rather than a passive observer. Express enthusiasm for the material where it is earned: booting the machine for the first time, watching the cursor blink, getting a syscall to return successfully — these are genuinely exciting moments and the prose should reflect that. Explain the *why* behind every decision; never leave the reader wondering why something is done a particular way. Anticipate confusion and address it directly ("this might look odd at first — here is why it works this way"). End every chapter with a "Where the Machine Is by the End of Chapter N" section that describes, in plain conversational terms, the new state the reader should hold in their head going into the next chapter.

7. **Discuss implementation concepts, not command references or API catalogues.** The book is about *how* operating system components are built — the data structures, the algorithms, the hardware interactions, the tradeoffs. It is not a user manual or an API reference. When documenting a component that exposes a user-visible interface (a shell, a filesystem, a syscall table), focus on *how it works inside* — the kernel structures involved, the decision algorithm, the hardware protocol — not on enumerating every flag, option, or error code. A shell chapter should explain how the shell resolves commands, manages its environment table, and connects processes through pipes; it should not be a list of built-in commands with their arguments. A syscall chapter should explain what the kernel does in response to each call, not reproduce the header-file definitions. If a complete reference is necessary, put it in a table at the end of the chapter, not as the chapter's primary content.

   **Rule of thumb for naming code.** Keep concrete names when they are part of the contract the reader should remember later — public syscalls, architectural instructions, standard structures, or user-visible interfaces such as `fork()`, `mmap()`, `SYS_*`, `termios`, `FILE`, or `int 0x80`. Demote names when they are only internal choreography and the reader mainly needs the idea rather than the helper name — for example `path_resolve`, `irq_dispatch`, `module_load`, `process_fork`, `paging_handle_fault`, or `tty_wake_readers`. In those cases, prefer prose like "the fault handler classifies the access and either materialises the page or delivers `SIGSEGV`" over a sentence that walks the reader through a chain of internal helper calls.

8. **Assume software engineering knowledge, not OS knowledge.** The reader is a working software engineer. They understand algorithms, data structures, pointers, memory allocation, compilation, and linking. They have never written an OS and do not know how a CPU boots, what a descriptor table is, how paging works, or what an interrupt is. Write to that asymmetry at all times: skip explaining what a pointer is; never skip explaining what the CPU does when an interrupt fires. When introducing any hardware mechanism or OS-specific concept, treat it as unknown to the reader regardless of how fundamental it may seem to an OS developer.

9. **Never use a concept before it has been introduced.** A reader should be able to read this book front to back without ever encountering a term, hardware detail, data structure, or mechanism that has not already been explained — either earlier in the current chapter or in a preceding chapter. This applies everywhere: prose, diagrams, code blocks, and captions. Before using a concept, introduce it: what it is, why it exists, and enough about how it works to follow the current discussion. Do not write "as we will see in Chapter N" or "see the section on X" to defer explaining something the reader needs right now. If a concept is prerequisite to the current chapter, either open the chapter by introducing it (if the explanation is short) or confirm it was fully covered in a prior chapter. When in doubt, re-introduce briefly rather than assuming retention.

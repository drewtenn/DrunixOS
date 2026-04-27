# Desktop Performance Roadmap

Date: 2026-04-27

Worktree: `/Users/drew/development/DrunixOS/.worktrees/desktop-snappy`

## Findings

1. Dragging repaints the entire desktop per mouse event.
   - Evidence: `handle_mouse_event()` sets `full_repaint` for every drag delta, and `present_scene()` recomposes the full scene then copies the full framebuffer.
   - Why it matters: at the default 1024x768x32 mode, each drag report can copy roughly 3 MiB from wallpaper to scene and 3 MiB from scene to framebuffer, before window and text rendering.
   - Proposed improvement: dirty-rect repaint for old and new window bounds plus old and new pointer bounds.

2. User-space `/dev/fb0` mappings do not inherit write-combining.
   - Evidence: the kernel identity framebuffer mapping is marked WC during boot, but user `mmap(/dev/fb0)` maps with `ARCH_MM_MAP_IO` only; x86 translates that to `PG_IO` without PAT/WC flags.
   - Why it matters: every compositor flush writes directly to mapped video memory. Without WC on the user mapping, framebuffer stores are slower and less burst-friendly.
   - Proposed improvement: represent framebuffer cache policy in the chardev mmap path and map user framebuffer PTEs with the same WC policy.

3. Userland `memcpy` is byte-at-a-time.
   - Evidence: `user/runtime/string.c` implements `memcpy()` as a single-byte loop, and `present_scene()` uses it for full framebuffer copies.
   - Why it matters: full-screen flushes issue one byte store per byte instead of wider stores. Even after dirty rects, pixel-copy paths still benefit from 32-bit-aligned copies.
   - Proposed improvement: add aligned word-copy paths to userland `memcpy()` and `memset()`, or use compositor-specific pixel row copy helpers.

4. Mouse events are rendered one record at a time.
   - Evidence: the event loop drains mouse records but calls `handle_mouse_event()` for each record, so drag backlog can repaint intermediate positions.
   - Why it matters: rendering stale intermediate drag positions wastes frame time and makes the pointer/window feel behind the current input.
   - Proposed improvement: coalesce pending mouse deltas and button state in the parent loop, then repaint once per event-drain cycle.

5. Files and Processes windows do filesystem work during repaint.
   - Evidence: `draw_dents_lines()` calls `sys_getdents()` whenever those app windows render.
   - Why it matters: if those windows are open during movement, each full repaint can repeat directory or procfs reads.
   - Proposed improvement: cache static app content and refresh on open/focus or explicit invalidation.

6. The desktop hot path builds with `-Og`.
   - Evidence: `user/Makefile` compiles user apps, including `desktop`, with debug-oriented optimization.
   - Why it matters: the compositor is a pixel-heavy loop; lower optimization leaves avoidable loop and call overhead in hot paths.
   - Proposed improvement: compile `desktop.o` and selected runtime pixel primitives with a targeted optimized flag.

## Current Plan

Implement item 1 first: dirty-rect repainting for window dragging. This directly removes the largest per-event cost and gives a clean foundation for the other improvements.

Detailed plan: `docs/superpowers/plans/2026-04-27-desktop-dirty-rect-drag.md`

## Baseline Note

`make check` built and ran the QEMU/user smoke tests, then failed at `check-test-intent-coverage` because `kernel/arch/x86/test/test_process.c` is missing the expected KTEST names `test_x86_user_layout_invariants` and `test_brk_refuses_stack_collision`. That failure was present before any roadmap or plan edits in this worktree.

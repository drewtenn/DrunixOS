# Desktop Performance Roadmap

Date: 2026-04-27

Worktree: `/Users/drew/development/DrunixOS/.worktrees/desktop-snappy`

## Findings

- [x] 1. Dragging repaints the entire desktop per mouse event.
  - Status: implemented.
  - Evidence: `user/apps/desktop.c` now has clipped composition and dirty presentation helpers (`compose_scene_rect()`, `present_dirty_rect()`), and drag movement repaints the union of old/new window bounds plus old/new pointer bounds. `shared/desktop_window.h` provides the shared rectangle helpers with KTEST coverage in `kernel/test/test_desktop_window.c`.
  - Why it matters: at the default 1024x768x32 mode, each drag report used to copy roughly 3 MiB from wallpaper to scene and 3 MiB from scene to framebuffer, before window and text rendering.

- [ ] 2. User-space `/dev/fb0` mappings do not inherit write-combining.
  - Status: not implemented.
  - Evidence: the kernel identity framebuffer mapping is marked WC during boot, but user `mmap(/dev/fb0)` still maps chardev pages with `ARCH_MM_MAP_IO` only; x86 still translates that to `PG_IO` without PAT/WC flags.
  - Why it matters: every compositor flush writes directly to mapped video memory. Without WC on the user mapping, framebuffer stores are slower and less burst-friendly.
  - Proposed improvement: represent framebuffer cache policy in the chardev mmap path and map user framebuffer PTEs with the same WC policy.

- [x] 3. Userland `memcpy` is byte-at-a-time.
  - Status: implemented.
  - Evidence: `user/runtime/string.c` now uses aligned `uint32_t` copy/fill loops in `memcpy()` and `memset()`, with byte fallbacks for unaligned prefixes and tails.
  - Why it matters: full-screen flushes no longer issue only one byte store per byte. Even after dirty rects, pixel-copy paths still benefit from 32-bit-aligned copies.

- [x] 4. Mouse events are rendered one record at a time.
  - Status: implemented.
  - Evidence: the mouse helper coalesces low-level `EV_REL`/`EV_KEY` records until `EV_SYN`, and the parent event loop now accumulates same-button `EVT_MOUSE` records with `drunix_mouse_coalesce_t`, flushing once per drained motion run. Button transitions flush pending motion first and are handled immediately so click and drag state changes remain observable.
  - Why it matters: rendering stale intermediate drag positions wastes frame time and makes the pointer/window feel behind the current input.

- [ ] 5. Files and Processes windows do filesystem work during repaint.
  - Status: not implemented.
  - Evidence: `draw_dents_lines()` still calls `sys_getdents()` whenever those app windows render.
  - Why it matters: if those windows are open during movement, each full repaint can repeat directory or procfs reads.
  - Proposed improvement: cache static app content and refresh on open/focus or explicit invalidation.

- [x] 6. The desktop hot path builds with `-Og`.
  - Status: implemented.
  - Evidence: `BUILD_MODE=production` is the default and uses `-O2` for x86 kernel C, x86 user C/C++, arm64 kernel C, and arm64 user C/C++. `BUILD_MODE=debug` uses `-Og` for those same binaries, and the `debug`, `debug-user`, and `debug-fresh` targets force that debug mode automatically. Build-mode sentinels force recompilation when the mode changes, and `tools/test_warning_policy.py` covers both modes in `make check`.
  - Why it matters: the compositor is a pixel-heavy loop; lower optimization leaves avoidable loop and call overhead in hot paths.
  - Proposed improvement: compile `desktop.o` and selected runtime pixel primitives with a targeted optimized flag.

## Current Plan

Items 1, 3, 4, and 6 are implemented.

Historical detailed plan for item 1: `docs/superpowers/plans/2026-04-27-desktop-dirty-rect-drag.md`

## Baseline Note

`make check` built and ran the QEMU/user smoke tests, then failed at `check-test-intent-coverage` because `kernel/arch/x86/test/test_process.c` is missing the expected KTEST names `test_x86_user_layout_invariants` and `test_brk_refuses_stack_collision`. That failure was present before any roadmap or plan edits in this worktree.

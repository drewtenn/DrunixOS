# Desktop Dirty Rect Drag Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make desktop window dragging snappier by repainting only the pixels affected by the moved window and pointer.

**Architecture:** Add small shared rectangle helpers, then teach the user-space compositor to compose and present a clipped dirty rectangle. Full-scene repaint remains the fallback for focus changes, taskbar actions, terminal output, close, minimize, and initial display.

**Tech Stack:** C, Drunix user runtime, shared desktop helpers, KTEST, QEMU-backed `make check`.

---

## File Structure

- Modify: `shared/desktop_window.h`
  - Add `drunix_rect_t` and inline rectangle helpers used by both KTEST and the compositor.
- Modify: `kernel/test/test_desktop_window.c`
  - Add unit coverage for rectangle validity, clipping, and union behavior.
- Modify: `user/apps/desktop.c`
  - Add compositor clip state.
  - Add dirty-rect wallpaper copy, scene composition, and presentation helpers.
  - Use dirty presentation for drag movement only.
- No docs changes are required for implementation beyond this plan.

## Behavior Contract

- Pointer-only movement keeps using the existing small pointer restoration path.
- Initial paint and non-drag state changes keep using `present_scene()`.
- Drag movement computes the old window rectangle before position update, the new window rectangle after clamping, and repaints the union of those rectangles plus old and new pointer rectangles.
- Clipped scene composition must leave `g_scene` correct inside the dirty rectangle and must not modify pixels outside that rectangle.
- The live framebuffer must not retain stale pointer pixels after a dirty repaint.

---

### Task 1: Add Shared Rectangle Helpers

**Files:**
- Modify: `shared/desktop_window.h`
- Test: `kernel/test/test_desktop_window.c`

- [ ] **Step 1: Write failing KTEST cases**

Add these test functions after `test_taskbar_hit_tests_all_rendered_apps()` in `kernel/test/test_desktop_window.c`:

```c
static void test_desktop_rect_clip_rejects_empty_and_outside(ktest_case_t *tc)
{
	drunix_rect_t bounds = drunix_rect_make(0, 0, 100, 80);
	drunix_rect_t empty = drunix_rect_make(10, 10, 0, 5);
	drunix_rect_t outside = drunix_rect_make(120, 10, 20, 20);
	drunix_rect_t clipped;

	KTEST_EXPECT_FALSE(tc, drunix_rect_valid(empty));
	KTEST_EXPECT_FALSE(tc, drunix_rect_clip(empty, bounds, &clipped));
	KTEST_EXPECT_FALSE(tc, drunix_rect_clip(outside, bounds, &clipped));
}

static void test_desktop_rect_clip_trims_to_bounds(ktest_case_t *tc)
{
	drunix_rect_t bounds = drunix_rect_make(0, 0, 100, 80);
	drunix_rect_t rect = drunix_rect_make(-10, 70, 30, 20);
	drunix_rect_t clipped;

	KTEST_EXPECT_TRUE(tc, drunix_rect_clip(rect, bounds, &clipped));
	KTEST_EXPECT_EQ(tc, clipped.x, 0);
	KTEST_EXPECT_EQ(tc, clipped.y, 70);
	KTEST_EXPECT_EQ(tc, clipped.w, 20);
	KTEST_EXPECT_EQ(tc, clipped.h, 10);
}

static void test_desktop_rect_union_covers_both_inputs(ktest_case_t *tc)
{
	drunix_rect_t a = drunix_rect_make(10, 12, 20, 30);
	drunix_rect_t b = drunix_rect_make(25, 5, 40, 10);
	drunix_rect_t merged = drunix_rect_union(a, b);

	KTEST_EXPECT_EQ(tc, merged.x, 10);
	KTEST_EXPECT_EQ(tc, merged.y, 5);
	KTEST_EXPECT_EQ(tc, merged.w, 55);
	KTEST_EXPECT_EQ(tc, merged.h, 37);
}
```

Add the cases to the existing `cases[]` list:

```c
	KTEST_CASE(test_desktop_rect_clip_rejects_empty_and_outside),
	KTEST_CASE(test_desktop_rect_clip_trims_to_bounds),
	KTEST_CASE(test_desktop_rect_union_covers_both_inputs),
```

- [ ] **Step 2: Run the focused check to verify failure**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile failure in `kernel/test/test_desktop_window.c` because `drunix_rect_t`, `drunix_rect_make`, `drunix_rect_valid`, `drunix_rect_clip`, and `drunix_rect_union` are not defined.

- [ ] **Step 3: Implement rectangle helpers**

Add this block in `shared/desktop_window.h` after the taskbar app constants:

```c
typedef struct {
	int x;
	int y;
	int w;
	int h;
} drunix_rect_t;

static inline drunix_rect_t drunix_rect_make(int x, int y, int w, int h)
{
	drunix_rect_t rect;

	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	return rect;
}

static inline int drunix_rect_valid(drunix_rect_t rect)
{
	return rect.w > 0 && rect.h > 0;
}

static inline int drunix_min_int(int a, int b)
{
	return a < b ? a : b;
}

static inline int drunix_max_int(int a, int b)
{
	return a > b ? a : b;
}

static inline drunix_rect_t
drunix_rect_union(drunix_rect_t a, drunix_rect_t b)
{
	int left;
	int top;
	int right;
	int bottom;

	if (!drunix_rect_valid(a))
		return b;
	if (!drunix_rect_valid(b))
		return a;

	left = drunix_min_int(a.x, b.x);
	top = drunix_min_int(a.y, b.y);
	right = drunix_max_int(a.x + a.w, b.x + b.w);
	bottom = drunix_max_int(a.y + a.h, b.y + b.h);
	return drunix_rect_make(left, top, right - left, bottom - top);
}

static inline int drunix_rect_clip(drunix_rect_t rect,
                                   drunix_rect_t bounds,
                                   drunix_rect_t *out)
{
	int left;
	int top;
	int right;
	int bottom;

	if (!out || !drunix_rect_valid(rect) || !drunix_rect_valid(bounds))
		return 0;

	left = drunix_max_int(rect.x, bounds.x);
	top = drunix_max_int(rect.y, bounds.y);
	right = drunix_min_int(rect.x + rect.w, bounds.x + bounds.w);
	bottom = drunix_min_int(rect.y + rect.h, bounds.y + bounds.h);
	if (right <= left || bottom <= top)
		return 0;

	*out = drunix_rect_make(left, top, right - left, bottom - top);
	return 1;
}
```

- [ ] **Step 4: Run the focused build**

Run:

```bash
make KTEST=1 kernel
```

Expected: PASS for compilation and linking of `kernel.elf`; no rectangle helper compile errors remain.

- [ ] **Step 5: Commit**

Run:

```bash
git add shared/desktop_window.h kernel/test/test_desktop_window.c
git commit -m "test: cover desktop dirty rectangle helpers"
```

---

### Task 2: Add Clipped Scene Composition

**Files:**
- Modify: `user/apps/desktop.c`

- [ ] **Step 1: Add compositor clip state**

Add these globals after `static uint32_t g_fb_bytes;`:

```c
static int g_clip_enabled;
static drunix_rect_t g_clip_rect;
```

Add these helper functions before `put_pixel()`:

```c
static drunix_rect_t screen_rect(void)
{
	return drunix_rect_make(0, 0, (int)g_info.width, (int)g_info.height);
}

static int point_inside_clip(int x, int y)
{
	if (!g_clip_enabled)
		return 1;
	return drunix_point_in_rect(x,
	                            y,
	                            g_clip_rect.x,
	                            g_clip_rect.y,
	                            g_clip_rect.w,
	                            g_clip_rect.h);
}

static void begin_clip(drunix_rect_t rect)
{
	g_clip_enabled = 1;
	g_clip_rect = rect;
}

static void end_clip(void)
{
	g_clip_enabled = 0;
}
```

Update `put_pixel()` so it returns before writing when the point is outside the active clip:

```c
static void put_pixel(uint32_t *target, int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 || x >= (int)g_info.width || y >= (int)g_info.height)
		return;
	if (!point_inside_clip(x, y))
		return;
	target[(uint32_t)y * g_pitch_pixels + (uint32_t)x] = color;
}
```

- [ ] **Step 2: Clip `fill_rect()` by rectangle instead of per pixel**

Replace `fill_rect()` with:

```c
static void
fill_rect(uint32_t *target, int x, int y, int w, int h, uint32_t color)
{
	drunix_rect_t rect;
	drunix_rect_t bounds;

	if (w <= 0 || h <= 0)
		return;
	rect = drunix_rect_make(x, y, w, h);
	bounds = screen_rect();
	if (g_clip_enabled)
		bounds = g_clip_rect;
	if (!drunix_rect_clip(rect, bounds, &rect))
		return;

	for (int j = 0; j < rect.h; j++) {
		uint32_t *row =
		    target + (uint32_t)(rect.y + j) * g_pitch_pixels + (uint32_t)rect.x;

		for (int i = 0; i < rect.w; i++)
			row[i] = color;
	}
}
```

- [ ] **Step 3: Add rectangle scene-copy helpers**

Add these functions after `copy_rect_from_scene()`:

```c
static void copy_wallpaper_rect_to_scene(drunix_rect_t rect)
{
	drunix_rect_t clipped;

	if (!g_scene || !g_wallpaper)
		return;
	if (!drunix_rect_clip(rect, screen_rect(), &clipped))
		return;

	for (int j = 0; j < clipped.h; j++) {
		uint32_t *src = g_wallpaper +
		    (uint32_t)(clipped.y + j) * g_pitch_pixels + (uint32_t)clipped.x;
		uint32_t *dst = g_scene +
		    (uint32_t)(clipped.y + j) * g_pitch_pixels + (uint32_t)clipped.x;

		memcpy(dst, src, (size_t)clipped.w * sizeof(uint32_t));
	}
}

static void copy_scene_rect_to_fb(drunix_rect_t rect)
{
	if (!drunix_rect_clip(rect, screen_rect(), &rect))
		return;
	copy_rect_from_scene(rect.x, rect.y, rect.w, rect.h);
}
```

- [ ] **Step 4: Add clipped compose and present helpers**

Add these functions after `compose_scene()`:

```c
static void compose_scene_rect(drunix_rect_t rect)
{
	drunix_rect_t clipped;

	if (!g_scene)
		return;
	if (!drunix_rect_clip(rect, screen_rect(), &clipped))
		return;

	if (g_wallpaper)
		copy_wallpaper_rect_to_scene(clipped);
	else
		render_wallpaper(g_scene);

	begin_clip(clipped);
	render_windows(g_scene);
	render_taskbar(g_scene);
	end_clip();
}

static drunix_rect_t pointer_rect_at(int x, int y)
{
	return drunix_rect_make(x, y, POINTER_W, POINTER_H);
}

static void present_dirty_rect(drunix_rect_t rect)
{
	drunix_rect_t dirty = rect;

	dirty = drunix_rect_union(dirty,
	                          pointer_rect_at(g_pointer_old_x, g_pointer_old_y));
	dirty = drunix_rect_union(dirty, pointer_rect_at(g_pointer_x, g_pointer_y));
	if (!drunix_rect_clip(dirty, screen_rect(), &dirty)) {
		render_pointer();
		return;
	}

	compose_scene_rect(dirty);
	copy_scene_rect_to_fb(dirty);
	draw_pointer_sprite();
	g_pointer_old_x = g_pointer_x;
	g_pointer_old_y = g_pointer_y;
}
```

- [ ] **Step 5: Build the desktop binary**

Run:

```bash
make NO_DESKTOP=0 kernel disk
```

Expected: build completes and `build/user/x86/bin/desktop` links.

- [ ] **Step 6: Commit**

Run:

```bash
git add user/apps/desktop.c
git commit -m "desktop: add clipped scene presentation"
```

---

### Task 3: Use Dirty Presentation While Dragging

**Files:**
- Modify: `user/apps/desktop.c`

- [ ] **Step 1: Add a helper for current window rectangle**

Add this function after `window_rect()`:

```c
static drunix_rect_t window_dirty_rect(int app)
{
	int x;
	int y;
	int w;
	int h;

	window_rect(app, &x, &y, &w, &h);
	return drunix_rect_make(x, y, w, h);
}
```

- [ ] **Step 2: Replace drag repaint state in `handle_mouse_event()`**

At the top of `handle_mouse_event()`, after `int full_repaint = 0;`, add:

```c
	int dirty_repaint = 0;
	drunix_rect_t dirty_rect = drunix_rect_make(0, 0, 0, 0);
```

Replace the drag movement body:

```c
				window_rect(g_dragging_app, &x, &y, &w, &h);
				set_window_position(g_dragging_app, x + dx, y + dy);
				clamp_window_position(g_dragging_app);
				full_repaint = 1;
```

with:

```c
				drunix_rect_t old_rect = window_dirty_rect(g_dragging_app);

				window_rect(g_dragging_app, &x, &y, &w, &h);
				set_window_position(g_dragging_app, x + dx, y + dy);
				clamp_window_position(g_dragging_app);
				dirty_rect =
				    drunix_rect_union(old_rect, window_dirty_rect(g_dragging_app));
				dirty_repaint = 1;
```

At the end of `handle_mouse_event()`, replace:

```c
	if (full_repaint)
		present_scene();
	else
		render_pointer();
```

with:

```c
	if (full_repaint)
		present_scene();
	else if (dirty_repaint)
		present_dirty_rect(dirty_rect);
	else
		render_pointer();
```

- [ ] **Step 3: Build and run the desktop image**

Run:

```bash
make NO_DESKTOP=0 kernel disk
```

Expected: build completes without warnings.

- [ ] **Step 4: Run the static desktop framework guard**

Run:

```bash
python3 tools/test_user_desktop_window_framework.py
```

Expected: exits 0 with no output.

- [ ] **Step 5: Run focused kernel build with desktop KTEST coverage**

Run:

```bash
make KTEST=1 kernel
```

Expected: build completes and `kernel.elf` links.

- [ ] **Step 6: Commit**

Run:

```bash
git add user/apps/desktop.c
git commit -m "desktop: repaint dirty window rects during drag"
```

---

### Task 4: Manual Drag Verification

**Files:**
- No source edits.

- [ ] **Step 1: Boot the desktop**

Run:

```bash
make NO_DESKTOP=0 run
```

Expected: QEMU boots to the framebuffer desktop with the shell in a terminal window.

- [ ] **Step 2: Verify drag behavior**

Use the QEMU window:

```text
Press and hold the terminal titlebar.
Move the mouse in small circles for at least five seconds.
Release the mouse button.
Open Files from the taskbar, drag it across the terminal, and release.
```

Expected:

```text
The pointer remains visible.
The dragged window follows the pointer without leaving stale pixels.
The old window position restores to wallpaper or underlying windows.
The taskbar remains intact when dragging near it.
The terminal continues to render text after dragging.
```

- [ ] **Step 3: Record verification result**

Append a short note to the commit message body of the final implementation commit, or add a follow-up verification commit only if the verification uncovered a source fix.

---

### Task 5: Final Verification

**Files:**
- No source edits.

- [ ] **Step 1: Run focused non-QEMU checks**

Run:

```bash
python3 tools/test_user_desktop_window_framework.py
make KTEST=1 kernel
make NO_DESKTOP=0 kernel disk
```

Expected:

```text
python3 tools/test_user_desktop_window_framework.py exits 0.
make KTEST=1 kernel exits 0.
make NO_DESKTOP=0 kernel disk exits 0.
```

- [ ] **Step 2: Run full check and document existing baseline failure**

Run:

```bash
make check
```

Expected:

```text
The target may still fail at check-test-intent-coverage because the baseline branch is missing:
kernel/arch/x86/test/test_process.c: test_x86_user_layout_invariants
kernel/arch/x86/test/test_process.c: test_brk_refuses_stack_collision
```

If `make check` fails for any new desktop, build, QEMU, shell, or KTEST reason before that known final guard, stop and fix that regression before completion.

---

## Self-Review

- Spec coverage: item 1 is covered by shared rect helpers, clipped scene composition, dirty drag presentation, manual verification, and focused build checks.
- Placeholder scan: this plan contains no placeholder tokens, no deferred implementation notes, and no unspecified test steps.
- Type consistency: `drunix_rect_t`, `drunix_rect_make`, `drunix_rect_valid`, `drunix_rect_union`, and `drunix_rect_clip` are introduced in Task 1 and used with the same names in later tasks.

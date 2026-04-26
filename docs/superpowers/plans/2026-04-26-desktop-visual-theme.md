# Desktop Visual Theme Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restyle the framebuffer desktop to the approved dark glass theme, add taskbar icons for real Drunix apps, and add real minimize/restore window-manager behavior.

**Architecture:** Keep the existing compositor in `kernel/gui/desktop.c` and extend its current window table instead of adding a second window model. Minimize is stored on each `desktop_window_t`; render and hit-test paths skip minimized windows, while taskbar and launcher paths restore them through a named helper. Visual changes stay in framebuffer drawing helpers and preserve the VGA/text fallback behavior.

**Tech Stack:** Drunix kernel C, existing framebuffer primitives, existing `ktest` kernel test suite, `make KTEST=1 kernel`, `make test-headless`, `make scan`.

---

## File Structure

- Modify `kernel/gui/desktop.h`: add `minimized` to `desktop_window_t`; declare `desktop_minimize_window` and `desktop_restore_window`.
- Modify `kernel/gui/desktop.c`: add minimize/restore helpers, minimize button geometry, minimized-window routing rules, wallpaper rendering, dark glass theme colors, taskbar icon drawing, and minimized taskbar rendering.
- Modify `kernel/test/test_desktop.c`: add behavior tests for minimize/restore and visual tests for wallpaper, chrome, taskbar icons, and unchanged launcher/taskbar behavior.
- Modify `docs/superpowers/specs/2026-04-26-desktop-visual-theme-design.md` only if implementation reveals a contradiction in the approved spec.

---

### Task 1: Add Real Minimized State And Restore API

**Files:**
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/gui/desktop.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing tests for minimized state and restore**

Add these tests near the existing taskbar/window tests in `kernel/test/test_desktop.c`:

```c
static void
test_desktop_minimize_keeps_window_open_and_focuses_next_visible(
    ktest_case_t *tc)
{
	desktop_state_t desktop;
	gui_display_t display;
	int files_id;
	int help_id;
	const desktop_window_t *help;

	gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
	desktop_init(&desktop, &display);
	files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
	help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
	KTEST_ASSERT_TRUE(tc, files_id > 0);
	KTEST_ASSERT_TRUE(tc, help_id > 0);

	KTEST_EXPECT_EQ(tc, desktop_minimize_window(&desktop, help_id), 1);
	help = desktop_window_for_test(&desktop, help_id);
	KTEST_ASSERT_NOT_NULL(tc, help);
	KTEST_EXPECT_TRUE(tc, help->minimized);
	KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 2);
	KTEST_EXPECT_EQ(
	    tc, desktop_focused_app_for_test(&desktop), DESKTOP_APP_FILES);

	desktop_test_destroy(&desktop);
}

static void
test_desktop_taskbar_click_restores_minimized_window(ktest_case_t *tc)
{
	desktop_state_t desktop;
	gui_display_t display;
	desktop_pointer_event_t ev;
	int files_id;
	int processes_id;
	const desktop_window_t *processes;

	gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
	desktop_init(&desktop, &display);
	files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
	processes_id = desktop_open_app_window(&desktop, DESKTOP_APP_PROCESSES);
	KTEST_ASSERT_TRUE(tc, files_id > 0);
	KTEST_ASSERT_TRUE(tc, processes_id > 0);
	KTEST_ASSERT_EQ(tc, desktop_minimize_window(&desktop, processes_id), 1);

	k_memset(&ev, 0, sizeof(ev));
	ev.left_down = 1;
	ev.x = 18;
	ev.y = desktop.taskbar.y;
	ev.pixel_x = ev.x * (int)GUI_FONT_W;
	ev.pixel_y = ev.y * (int)GUI_FONT_H;
	desktop_handle_pointer(&desktop, &ev);

	processes = desktop_window_for_test(&desktop, processes_id);
	KTEST_ASSERT_NOT_NULL(tc, processes);
	KTEST_EXPECT_FALSE(tc, processes->minimized);
	KTEST_EXPECT_EQ(
	    tc, desktop_focused_app_for_test(&desktop), DESKTOP_APP_PROCESSES);

	desktop_test_destroy(&desktop);
}

static void
test_desktop_open_existing_minimized_app_restores_without_duplicate(
    ktest_case_t *tc)
{
	desktop_state_t desktop;
	gui_display_t display;
	int files_id;
	int reopened_id;
	const desktop_window_t *files;

	gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
	desktop_init(&desktop, &display);
	files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
	KTEST_ASSERT_TRUE(tc, files_id > 0);
	KTEST_ASSERT_EQ(tc, desktop_minimize_window(&desktop, files_id), 1);

	reopened_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);

	files = desktop_window_for_test(&desktop, files_id);
	KTEST_ASSERT_NOT_NULL(tc, files);
	KTEST_EXPECT_EQ(tc, reopened_id, files_id);
	KTEST_EXPECT_FALSE(tc, files->minimized);
	KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 1);
	KTEST_EXPECT_EQ(
	    tc, desktop_focused_app_for_test(&desktop), DESKTOP_APP_FILES);

	desktop_test_destroy(&desktop);
}

static void
test_desktop_minimized_shell_preserves_process_output(ktest_case_t *tc)
{
	desktop_state_t desktop;
	gui_display_t display;
	int shell_id;
	const desktop_window_t *shell;

	gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
	desktop_init(&desktop, &display);
	shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
	KTEST_ASSERT_TRUE(tc, shell_id > 0);
	desktop_attach_shell_process(&desktop, 77, 77);

	KTEST_ASSERT_EQ(tc, desktop_minimize_window(&desktop, shell_id), 1);
	KTEST_EXPECT_TRUE(tc, desktop_process_owns_shell(&desktop, 77, 77));
	KTEST_EXPECT_EQ(
	    tc, desktop_write_process_output(&desktop, 77, 77, "A", 1), 1);
	KTEST_ASSERT_EQ(tc, desktop_restore_window(&desktop, shell_id), 1);

	shell = desktop_window_for_test(&desktop, shell_id);
	KTEST_ASSERT_NOT_NULL(tc, shell);
	KTEST_EXPECT_FALSE(tc, shell->minimized);
	KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&desktop.shell_terminal, 0, 0).ch,
	                'A');
	KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'b'), DESKTOP_KEY_FORWARD);

	desktop_test_destroy(&desktop);
}
```

Add these cases to the `cases[]` table after `test_desktop_taskbar_shell_refocus_forwards_keys`:

```c
    KTEST_CASE(test_desktop_minimize_keeps_window_open_and_focuses_next_visible),
    KTEST_CASE(test_desktop_taskbar_click_restores_minimized_window),
    KTEST_CASE(
        test_desktop_open_existing_minimized_app_restores_without_duplicate),
    KTEST_CASE(test_desktop_minimized_shell_preserves_process_output),
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails because `desktop_window_t` has no `minimized` member and `desktop_minimize_window` / `desktop_restore_window` are undeclared.

- [ ] **Step 3: Add public state and helper declarations**

In `kernel/gui/desktop.h`, add `minimized` after `focused` in `desktop_window_t`:

```c
	int minimized;
```

Add declarations after `desktop_close_window`:

```c
int desktop_minimize_window(desktop_state_t *desktop, int window_id);
int desktop_restore_window(desktop_state_t *desktop, int window_id);
```

- [ ] **Step 4: Implement minimize/restore state transitions**

In `kernel/gui/desktop.c`, add this helper after `desktop_find_topmost_open_window`:

```c
static desktop_window_t *
desktop_find_topmost_visible_window(desktop_state_t *desktop)
{
	desktop_window_t *best = 0;

	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		desktop_window_t *win = &desktop->windows[i];

		if (!win->open || win->minimized)
			continue;
		if (!best || win->z > best->z)
			best = win;
	}
	return best;
}
```

In `desktop_focus_window`, restore minimized targets before focusing:

```c
	target = desktop_find_window(desktop, window_id);
	if (!target)
		return;
	target->minimized = 0;
```

In `desktop_open_app_window`, change the existing-window branch to restore instead of leaving minimized windows hidden:

```c
	if (existing) {
		if (app == DESKTOP_APP_SHELL) {
			existing->rect = desktop->window_pixel_rect;
			desktop_clamp_window_rect(desktop, &existing->rect);
			desktop_update_window_content_rect(existing);
			desktop_sync_shell_geometry(desktop, existing);
			desktop->shell_window_open = 1;
		}
		desktop_restore_window(desktop, existing->id);
		return existing->id;
	}
```

Add the two public helpers before `desktop_close_window`:

```c
int desktop_restore_window(desktop_state_t *desktop, int window_id)
{
	desktop_window_t *win;

	if (!desktop)
		return 0;
	win = desktop_find_window(desktop, window_id);
	if (!win)
		return 0;
	win->minimized = 0;
	desktop_focus_window(desktop, window_id);
	return 1;
}

int desktop_minimize_window(desktop_state_t *desktop, int window_id)
{
	desktop_window_t *win;
	desktop_window_t *next;

	if (!desktop)
		return 0;
	win = desktop_find_window(desktop, window_id);
	if (!win)
		return 0;
	if (desktop->dragging_window_id == window_id)
		desktop->dragging_window_id = 0;
	win->minimized = 1;
	win->focused = 0;
	if (desktop->focused_window_id != window_id)
		return 1;

	desktop->focused_window_id = 0;
	next = desktop_find_topmost_visible_window(desktop);
	if (next) {
		desktop_focus_window(desktop, next->id);
	} else {
		desktop->focus = DESKTOP_FOCUS_TASKBAR;
	}
	return 1;
}
```

In `desktop_close_window`, replace the fallback window lookup:

```c
	if (!next)
		next = desktop_find_topmost_visible_window(desktop);
```

In `desktop_handle_pointer`, update the taskbar branch:

```c
		if (task_win) {
			if (task_win->minimized)
				desktop_restore_window(desktop, task_win->id);
			else
				desktop_focus_window(desktop, task_win->id);
			desktop_render(desktop);
			return;
		}
```

- [ ] **Step 5: Run tests to verify they pass**

Run:

```bash
make KTEST=1 kernel
make test-headless
```

Expected: kernel compiles and `logs/debugcon-ktest.log` contains `KTEST: SUMMARY pass=` with `fail=0`.

- [ ] **Step 6: Commit**

```bash
git add kernel/gui/desktop.h kernel/gui/desktop.c kernel/test/test_desktop.c
git commit -m "feat: add desktop window minimize state"
```

---

### Task 2: Add Title-Bar Minimize Button And Minimized Hit/Render Rules

**Files:**
- Modify: `kernel/gui/desktop.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing tests for title-bar minimize and hidden-window hit testing**

Add these tests after `test_desktop_shell_close_button_closes_visible_shell_window`:

```c
static void
test_desktop_minimize_button_minimizes_files_window(ktest_case_t *tc)
{
	desktop_state_t desktop;
	gui_display_t display;
	desktop_pointer_event_t ev;
	int files_id;
	const desktop_window_t *win;

	gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
	desktop_init(&desktop, &display);
	files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
	win = desktop_window_for_test(&desktop, files_id);
	KTEST_ASSERT_NOT_NULL(tc, win);

	k_memset(&ev, 0, sizeof(ev));
	ev.left_down = 1;
	ev.pixel_x = win->rect.x + win->rect.w - 28;
	ev.pixel_y = win->rect.y + 8;
	ev.x = ev.pixel_x / (int)GUI_FONT_W;
	ev.y = ev.pixel_y / (int)GUI_FONT_H;
	desktop_handle_pointer(&desktop, &ev);

	win = desktop_window_for_test(&desktop, files_id);
	KTEST_ASSERT_NOT_NULL(tc, win);
	KTEST_EXPECT_TRUE(tc, win->minimized);
	KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 1);

	desktop_test_destroy(&desktop);
}

static void
test_desktop_minimized_top_window_does_not_receive_pointer_focus(
    ktest_case_t *tc)
{
	desktop_state_t desktop;
	gui_display_t display;
	desktop_pointer_event_t ev;
	int files_id;
	int help_id;
	const desktop_window_t *help;

	gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
	desktop_init(&desktop, &display);
	files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
	help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
	KTEST_ASSERT_TRUE(tc, files_id > 0);
	KTEST_ASSERT_TRUE(tc, help_id > 0);
	KTEST_ASSERT_EQ(tc, desktop_minimize_window(&desktop, help_id), 1);
	help = desktop_window_for_test(&desktop, help_id);
	KTEST_ASSERT_NOT_NULL(tc, help);

	k_memset(&ev, 0, sizeof(ev));
	ev.left_down = 1;
	ev.pixel_x = help->rect.x + 20;
	ev.pixel_y = help->rect.y + 30;
	ev.x = ev.pixel_x / (int)GUI_FONT_W;
	ev.y = ev.pixel_y / (int)GUI_FONT_H;
	desktop_handle_pointer(&desktop, &ev);

	KTEST_EXPECT_EQ(
	    tc, desktop_focused_app_for_test(&desktop), DESKTOP_APP_FILES);

	desktop_test_destroy(&desktop);
}
```

Add both to `cases[]` after `test_desktop_shell_close_button_closes_visible_shell_window`:

```c
    KTEST_CASE(test_desktop_minimize_button_minimizes_files_window),
    KTEST_CASE(test_desktop_minimized_top_window_does_not_receive_pointer_focus),
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
make test-headless
```

Expected: `test_desktop_minimize_button_minimizes_files_window` fails because the click starts a drag or does nothing; `fail=` is nonzero.

- [ ] **Step 3: Implement minimize button geometry and routing**

In `kernel/gui/desktop.c`, add constants near the close-button constants:

```c
#define DESKTOP_WINDOW_BUTTON_GAP 4
```

Add this helper after `desktop_point_in_close_button`:

```c
static int
desktop_point_in_minimize_button(const desktop_window_t *win, int x, int y)
{
	gui_pixel_rect_t minimize_button;

	if (!win)
		return 0;
	minimize_button.x = win->rect.x + win->rect.w -
	                    DESKTOP_WINDOW_CLOSE_BUTTON_W -
	                    DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN -
	                    DESKTOP_WINDOW_BUTTON_GAP -
	                    DESKTOP_WINDOW_CLOSE_BUTTON_W;
	minimize_button.y = win->rect.y + DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN;
	minimize_button.w = DESKTOP_WINDOW_CLOSE_BUTTON_W;
	minimize_button.h = DESKTOP_WINDOW_CLOSE_BUTTON_H;
	return desktop_point_in_pixel_rect(x, y, &minimize_button);
}
```

In `desktop_top_window_at`, skip minimized windows:

```c
		if (!win->open || win->minimized)
			continue;
```

In `desktop_next_window_by_z`, skip minimized windows for rendering:

```c
		if (!win->open || win->minimized || win->z <= min_z)
			continue;
```

In `desktop_clip_fully_covered_by_window`, skip minimized windows:

```c
		if (!win->open || win->minimized || win->rect.w <= 0 ||
		    win->rect.h <= 0)
			continue;
```

In `desktop_shell_content_has_overlap_above`, skip minimized windows:

```c
		if (!win->open || win->minimized || win->id == shell_win->id ||
		    win->z <= shell_win->z)
			continue;
```

In `desktop_render_framebuffer_window`, return early for minimized windows:

```c
	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer ||
	    !clip || !theme || !win || !win->open || win->minimized)
		return;
```

In `desktop_render_framebuffer_terminal`, skip hidden shell windows:

```c
	if (!shell_win || !shell_win->open || shell_win->minimized)
		return;
```

In `desktop_handle_pointer`, add minimize handling between close handling and title drag:

```c
		if (desktop_point_in_minimize_button(
		        top, pointer_pixel_x, pointer_pixel_y)) {
			desktop_minimize_window(desktop, top->id);
			desktop_render(desktop);
			return;
		}
```

- [ ] **Step 4: Draw minimize button in framebuffer and text paths**

In `desktop_render_framebuffer_window`, draw `-` before the close button:

```c
	framebuffer_draw_text_clipped(
	    fb,
	    clip,
	    win->rect.x + win->rect.w - DESKTOP_WINDOW_CLOSE_BUTTON_W -
	        DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN - DESKTOP_WINDOW_BUTTON_GAP -
	        DESKTOP_WINDOW_CLOSE_BUTTON_W,
	    win->rect.y + DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN,
	    "-",
	    theme->title_fg,
	    theme->title_bg);
```

In the text-mode shell frame inside `desktop_render`, draw `-` next to `x` only when the shell window is visible:

```c
		gui_display_draw_text(desktop->display,
		                      desktop->shell_rect.x + desktop->shell_rect.w - 5,
		                      desktop->shell_rect.y,
		                      1,
		                      "-",
		                      DESKTOP_ATTR_WINDOW);
```

Also gate text-mode shell frame drawing and `desktop_shell_redraw(desktop)` behind a visible shell window:

```c
	desktop_window_t *shell_win = desktop_find_app_window(desktop,
	                                                      DESKTOP_APP_SHELL);
	int shell_visible = shell_win && shell_win->open && !shell_win->minimized;
```

Use `shell_visible` instead of `desktop->shell_window_open` for text rendering and shell redraw.

- [ ] **Step 5: Run tests to verify they pass**

Run:

```bash
make test-headless
```

Expected: all KTEST cases pass with `fail=0`.

- [ ] **Step 6: Commit**

```bash
git add kernel/gui/desktop.c kernel/test/test_desktop.c
git commit -m "feat: add desktop window minimize button"
```

---

### Task 3: Apply Dark Glass Framebuffer Theme And Wallpaper

**Files:**
- Modify: `kernel/gui/desktop.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing visual theme tests**

Add this test after `test_desktop_help_app_render_is_visible_in_framebuffer`:

```c
static void
test_desktop_framebuffer_uses_dark_blue_wallpaper_and_taskbar(
    ktest_case_t *tc)
{
	static uint32_t pixels[480 * 400];
	gui_display_t display;
	desktop_state_t desktop;
	framebuffer_info_t fb;
	uint32_t wallpaper;
	uint32_t taskbar;
	int taskbar_y;

	k_memset(pixels, 0, sizeof(pixels));
	k_memset(&fb, 0, sizeof(fb));
	fb.address = (uintptr_t)pixels;
	fb.pitch = 480u * sizeof(uint32_t);
	fb.width = 480u;
	fb.height = 400u;
	fb.bpp = 32u;
	fb.red_pos = 16u;
	fb.red_size = 8u;
	fb.green_pos = 8u;
	fb.green_size = 8u;
	fb.blue_pos = 0u;
	fb.blue_size = 8u;

	gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
	desktop_init(&desktop, &display);
	desktop_set_framebuffer_target(&desktop, &fb);
	desktop_render(&desktop);

	wallpaper = framebuffer_pack_rgb(&fb, 0x07, 0x1c, 0x3a);
	taskbar = framebuffer_pack_rgb(&fb, 0x06, 0x11, 0x1f);
	taskbar_y = desktop.taskbar_pixel_rect.y + 4;
	KTEST_EXPECT_EQ(tc, pixels[10 * 480 + 10], wallpaper);
	KTEST_EXPECT_EQ(tc, pixels[taskbar_y * 480 + 10], taskbar);

	desktop_test_destroy(&desktop);
}
```

Update `test_desktop_help_app_render_is_visible_in_framebuffer` so the expected content background is:

```c
	bg = framebuffer_pack_rgb(&fb, 0x17, 0x28, 0x3a);
```

Add the new case near the other framebuffer render tests:

```c
    KTEST_CASE(test_desktop_framebuffer_uses_dark_blue_wallpaper_and_taskbar),
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
make test-headless
```

Expected: visual tests fail because the old warm palette still renders.

- [ ] **Step 3: Update framebuffer theme colors**

Replace `desktop_pixel_theme` with these color assignments:

```c
static gui_pixel_theme_t desktop_pixel_theme(const framebuffer_info_t *fb)
{
	gui_pixel_theme_t theme;

	k_memset(&theme, 0, sizeof(theme));
	theme.desktop_bg = framebuffer_pack_rgb(fb, 0x07, 0x1c, 0x3a);
	theme.taskbar_bg = framebuffer_pack_rgb(fb, 0x06, 0x11, 0x1f);
	theme.taskbar_fg = framebuffer_pack_rgb(fb, 0xe8, 0xf1, 0xf7);
	theme.window_bg = framebuffer_pack_rgb(fb, 0x17, 0x28, 0x3a);
	theme.window_border = framebuffer_pack_rgb(fb, 0x3b, 0xaf, 0xda);
	theme.title_bg = framebuffer_pack_rgb(fb, 0x22, 0x36, 0x4b);
	theme.title_fg = framebuffer_pack_rgb(fb, 0xee, 0xf6, 0xff);
	theme.terminal_bg = framebuffer_pack_rgb(fb, 0x09, 0x11, 0x1d);
	theme.terminal_fg = framebuffer_pack_rgb(fb, 0xe8, 0xf1, 0xf7);
	theme.terminal_dim = framebuffer_pack_rgb(fb, 0x7f, 0x9b, 0xad);
	theme.terminal_cursor = framebuffer_pack_rgb(fb, 0x55, 0xc7, 0xf2);
	theme.scrollbar_track = framebuffer_pack_rgb(fb, 0x0d, 0x1b, 0x2c);
	theme.scrollbar_thumb = framebuffer_pack_rgb(fb, 0x4f, 0xc3, 0xf7);
	return theme;
}
```

- [ ] **Step 4: Add clipped wallpaper drawing**

Add this helper before `desktop_render_framebuffer_region`:

```c
static void desktop_render_framebuffer_wallpaper(const framebuffer_info_t *fb,
                                                 const gui_pixel_rect_t *clip,
                                                 const gui_pixel_theme_t *theme)
{
	uint32_t band_a;
	uint32_t band_b;
	int x0;
	int y0;
	int x1;
	int y1;

	if (!fb || !clip || !theme)
		return;
	desktop_pixel_fill_rect(
	    fb, clip, 0, 0, (int)fb->width, (int)fb->height, theme->desktop_bg);

	band_a = framebuffer_pack_rgb(fb, 0x0d, 0x6e, 0x9f);
	band_b = framebuffer_pack_rgb(fb, 0x11, 0x8d, 0x8d);
	x0 = clip->x < 0 ? 0 : clip->x;
	y0 = clip->y < 0 ? 0 : clip->y;
	x1 = clip->x + clip->w;
	y1 = clip->y + clip->h;
	if (x1 > (int)fb->width)
		x1 = (int)fb->width;
	if (y1 > (int)fb->height)
		y1 = (int)fb->height;

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			int diagonal = x - y;

			if (diagonal > 40 && diagonal < 70 && y > (int)fb->height / 3)
				framebuffer_fill_rect(fb, x, y, 1, 1, band_b);
			else if (diagonal > 130 && diagonal < 165)
				framebuffer_fill_rect(fb, x, y, 1, 1, band_a);
		}
	}
}
```

In `desktop_render_framebuffer_region`, replace the flat background fill:

```c
	if (!desktop_clip_fully_covered_by_window(desktop, clip))
		desktop_render_framebuffer_wallpaper(fb, clip, &theme);
```

- [ ] **Step 5: Add taskbar highlight and window chrome depth**

After taskbar fill in `desktop_render_framebuffer_region`, add:

```c
	desktop_pixel_fill_rect(fb,
	                        clip,
	                        desktop->taskbar_pixel_rect.x,
	                        desktop->taskbar_pixel_rect.y,
	                        desktop->taskbar_pixel_rect.w,
	                        1,
	                        framebuffer_pack_rgb(fb, 0x24, 0x52, 0x72));
```

In `desktop_render_framebuffer_window`, add a title-bar lower highlight after filling the title bar:

```c
	desktop_pixel_fill_rect(fb,
	                        clip,
	                        win->rect.x,
	                        win->rect.y + DESKTOP_WINDOW_CHROME_H - 1,
	                        win->rect.w,
	                        1,
	                        framebuffer_pack_rgb(fb, 0x35, 0x5d, 0x7c));
```

- [ ] **Step 6: Run tests to verify they pass**

Run:

```bash
make test-headless
```

Expected: all KTEST cases pass with `fail=0`.

- [ ] **Step 7: Commit**

```bash
git add kernel/gui/desktop.c kernel/test/test_desktop.c
git commit -m "style: apply dark desktop framebuffer theme"
```

---

### Task 4: Draw Real Drunix Taskbar Icons

**Files:**
- Modify: `kernel/gui/desktop.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing taskbar icon tests**

Add this test after the dark wallpaper test:

```c
static void test_desktop_framebuffer_draws_taskbar_icons(ktest_case_t *tc)
{
	static uint32_t pixels[480 * 400];
	gui_display_t display;
	desktop_state_t desktop;
	framebuffer_info_t fb;
	uint32_t taskbar;
	int files_id;
	int taskbar_y;

	k_memset(pixels, 0, sizeof(pixels));
	k_memset(&fb, 0, sizeof(fb));
	fb.address = (uintptr_t)pixels;
	fb.pitch = 480u * sizeof(uint32_t);
	fb.width = 480u;
	fb.height = 400u;
	fb.bpp = 32u;
	fb.red_pos = 16u;
	fb.red_size = 8u;
	fb.green_pos = 8u;
	fb.green_size = 8u;
	fb.blue_pos = 0u;
	fb.blue_size = 8u;

	gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
	desktop_init(&desktop, &display);
	desktop_set_framebuffer_target(&desktop, &fb);
	files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
	KTEST_ASSERT_TRUE(tc, files_id > 0);
	desktop_render(&desktop);

	taskbar = framebuffer_pack_rgb(&fb, 0x06, 0x11, 0x1f);
	taskbar_y = desktop.taskbar_pixel_rect.y + 8;
	KTEST_EXPECT_NE(tc, pixels[taskbar_y * 480 + 24], taskbar);
	KTEST_EXPECT_NE(tc, pixels[taskbar_y * 480 + 80], taskbar);

	desktop_test_destroy(&desktop);
}
```

Add it to `cases[]` after `test_desktop_framebuffer_uses_dark_blue_wallpaper_and_taskbar`:

```c
    KTEST_CASE(test_desktop_framebuffer_draws_taskbar_icons),
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
make test-headless
```

Expected: icon pixel checks fail because the taskbar currently only draws text.

- [ ] **Step 3: Add icon drawing helper**

Add this helper before `desktop_render_framebuffer_region`:

```c
static void desktop_draw_taskbar_icon(const framebuffer_info_t *fb,
                                      const gui_pixel_rect_t *clip,
                                      desktop_app_kind_t app,
                                      int x,
                                      int y,
                                      uint32_t fg,
                                      uint32_t accent)
{
	if (!fb || !clip)
		return;

	desktop_pixel_fill_rect(fb, clip, x, y, 18, 18, framebuffer_pack_rgb(fb, 0x12, 0x25, 0x3a));
	desktop_pixel_draw_outline(fb, clip, x, y, 18, 18, accent);
	if (app == DESKTOP_APP_SHELL) {
		framebuffer_draw_text_clipped(fb, clip, x + 4, y + 1, ">", fg,
		                              framebuffer_pack_rgb(fb, 0x12, 0x25, 0x3a));
		desktop_pixel_fill_rect(fb, clip, x + 10, y + 13, 5, 1, fg);
	} else if (app == DESKTOP_APP_FILES) {
		desktop_pixel_fill_rect(fb, clip, x + 3, y + 5, 12, 9, accent);
		desktop_pixel_fill_rect(fb, clip, x + 4, y + 3, 6, 3, fg);
	} else if (app == DESKTOP_APP_PROCESSES) {
		desktop_pixel_draw_outline(fb, clip, x + 3, y + 5, 12, 8, accent);
		desktop_pixel_fill_rect(fb, clip, x + 5, y + 10, 2, 2, fg);
		desktop_pixel_fill_rect(fb, clip, x + 8, y + 7, 2, 5, fg);
		desktop_pixel_fill_rect(fb, clip, x + 11, y + 9, 2, 3, fg);
	} else if (app == DESKTOP_APP_HELP) {
		desktop_pixel_fill_rect(fb, clip, x + 5, y + 3, 8, 12, fg);
		framebuffer_draw_text_clipped(fb, clip, x + 7, y + 2, "?", accent, fg);
	} else {
		framebuffer_draw_text_clipped(fb, clip, x + 4, y + 1, "D", fg,
		                              framebuffer_pack_rgb(fb, 0x12, 0x25, 0x3a));
		desktop_pixel_fill_rect(fb, clip, x + 7, y + 12, 7, 2, accent);
	}
}
```

- [ ] **Step 4: Draw launcher and open-window icons**

In `desktop_render_framebuffer_region`, replace the taskbar `Menu` draw call with:

```c
	desktop_draw_taskbar_icon(fb,
	                          clip,
	                          DESKTOP_APP_NONE,
	                          desktop->taskbar_pixel_rect.x + 16,
	                          desktop->taskbar_pixel_rect.y + 2,
	                          theme.taskbar_fg,
	                          theme.window_border);
```

In the open-window taskbar loop, draw each icon and shift the label:

```c
		int task_x = desktop->taskbar_pixel_rect.x + 72;

		for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
			uint32_t label_fg;

			if (!desktop->windows[i].open)
				continue;
			label_fg = desktop->windows[i].minimized
			               ? theme.terminal_dim
			               : theme.taskbar_fg;
			desktop_draw_taskbar_icon(fb,
			                          clip,
			                          desktop->windows[i].app,
			                          task_x,
			                          desktop->taskbar_pixel_rect.y + 2,
			                          label_fg,
			                          theme.window_border);
			framebuffer_draw_text_clipped(fb,
			                              clip,
			                              task_x + 24,
			                              desktop->taskbar_pixel_rect.y + 2,
			                              desktop->windows[i].title,
			                              label_fg,
			                              theme.taskbar_bg);
			task_x += 96;
		}
```

- [ ] **Step 5: Keep text fallback unchanged**

Do not add pixel icons to the text fallback. Its existing `Menu` and window labels remain the VGA representation.

- [ ] **Step 6: Run tests to verify they pass**

Run:

```bash
make test-headless
```

Expected: all KTEST cases pass with `fail=0`.

- [ ] **Step 7: Commit**

```bash
git add kernel/gui/desktop.c kernel/test/test_desktop.c
git commit -m "style: draw desktop taskbar icons"
```

---

### Task 5: Final Verification And Cleanup

**Files:**
- Review: `kernel/gui/desktop.h`
- Review: `kernel/gui/desktop.c`
- Review: `kernel/test/test_desktop.c`
- Review: `docs/superpowers/specs/2026-04-26-desktop-visual-theme-design.md`

- [ ] **Step 1: Run full headless kernel tests**

Run:

```bash
make test-headless
```

Expected: command exits 0 and `logs/debugcon-ktest.log` contains `KTEST: SUMMARY pass=` with `fail=0`.

- [ ] **Step 2: Run mechanical scan**

Run:

```bash
make scan
```

Expected: command exits 0. If it reports pre-existing unrelated issues, capture the output and run the narrowest relevant command that still verifies touched files, such as `make format-check`.

- [ ] **Step 3: Inspect final diff**

Run:

```bash
git diff -- kernel/gui/desktop.h kernel/gui/desktop.c kernel/test/test_desktop.c
```

Expected: diff shows only the approved desktop theme, taskbar icon, and minimize/restore changes.

- [ ] **Step 4: Commit final cleanup if needed**

If Step 2 or Step 3 required any cleanup edits, commit them:

```bash
git add kernel/gui/desktop.h kernel/gui/desktop.c kernel/test/test_desktop.c
git commit -m "fix: clean up desktop theme implementation"
```

If no cleanup edits were required, do not create an empty commit.

#include "desktop.h"
#include "desktop_apps.h"
#include "kheap.h"
#include "kstring.h"
#include "paging.h"
#include <limits.h>

#define DESKTOP_ATTR_BACKGROUND 0x1f
#define DESKTOP_ATTR_TASKBAR 0x70
#define DESKTOP_ATTR_WINDOW 0x1e
#define DESKTOP_ATTR_TITLE 0x70
#define DESKTOP_ATTR_LAUNCHER 0x70
#define DESKTOP_CURSOR_W 8
#define DESKTOP_CURSOR_H 12
#define DESKTOP_TERMINAL_HISTORY_ROWS 500
#define DESKTOP_TERMINAL_PADDING_X 8
#define DESKTOP_TERMINAL_PADDING_Y 6
#define DESKTOP_WINDOW_CHROME_H 20
#define DESKTOP_WINDOW_CLOSE_BUTTON_W 12
#define DESKTOP_WINDOW_CLOSE_BUTTON_H 12
#define DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN 4

static desktop_state_t *g_desktop = 0;
static int g_framebuffer_present_depth = 0;
static uint32_t g_framebuffer_present_saved_cr3 = 0;

static uint32_t desktop_read_cr3(void)
{
	uint32_t cr3;

	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	return cr3;
}

static void desktop_write_cr3(uint32_t cr3)
{
	__asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

/*
 * Framebuffer present critical section.
 *
 * begin() saves EFLAGS and disables interrupts; end() restores the prior
 * IF state. Calls nest safely via g_framebuffer_present_depth, which the
 * KTEST scroll-interleave hook also uses to detect re-entry.
 *
 * Why we mask interrupts:
 *
 * Historically this existed so a mouse IRQ firing mid-render couldn't
 * blit its cursor sprite into the visible framebuffer on top of a
 * partially-drawn window, leaving a cursor ghost. That specific problem
 * is now impossible — the cursor is a hardware-style overlay that's
 * composited during framebuffer_present_rect(), never written into any
 * buffer by the IRQ path — but three other races still need this guard:
 *
 *  1. Back-buffer write races. Rasterisation (fill/blit/glyph) and the
 *     back→front memcpy in framebuffer_present_rect() both touch the
 *     back buffer with non-atomic memmoves. A mouse or keyboard IRQ
 *     that re-entered the present path would interleave its back-buffer
 *     writes and reads with the main path's, producing torn pixels in
 *     the flushed output.
 *
 *  2. Flush-vs-write ordering. The flush reads the back buffer while
 *     the main path may still be in the middle of composing the frame.
 *     Without this guard a cursor-motion flush could copy a half-
 *     composed region to the visible framebuffer.
 *
 *  3. Desktop-state mutation mid-render. desktop_handle_pointer() (from
 *     the mouse IRQ) and desktop_handle_key() mutate dragging_window_id,
 *     focused_window_id, per-window rects, and pointer coordinates. If
 *     those changed mid-walk inside desktop_render_framebuffer_region()
 *     the z-order traversal would see inconsistent state and emit a
 *     frame with half-old, half-new window geometry.
 *
 * Finer-grained locks would let the timer and keyboard IRQs continue to
 * fire, but with only a handful of windows and no real preemption the
 * blanket cli/sti is simpler and cheap enough — the critical section is
 * at most a small dirty-rect union of rasterise + memcpy.
 */
static uint32_t desktop_framebuffer_present_begin(void)
{
	uint32_t flags;

	__asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
	if (g_framebuffer_present_depth == 0) {
		g_framebuffer_present_saved_cr3 = desktop_read_cr3();
		if (g_framebuffer_present_saved_cr3 != PAGE_DIR_ADDR)
			desktop_write_cr3(PAGE_DIR_ADDR);
	}
	g_framebuffer_present_depth++;
	return flags;
}

static void desktop_framebuffer_present_end(uint32_t flags)
{
	if (g_framebuffer_present_depth > 0)
		g_framebuffer_present_depth--;
	if (g_framebuffer_present_depth == 0 &&
	    g_framebuffer_present_saved_cr3 != 0 &&
	    g_framebuffer_present_saved_cr3 != PAGE_DIR_ADDR) {
		desktop_write_cr3(g_framebuffer_present_saved_cr3);
	}
	if (flags & (1u << 9))
		__asm__ volatile("sti" ::: "memory");
}

#ifdef KTEST_ENABLED
static desktop_scroll_interleave_hook_t g_scroll_interleave_hook = 0;

void desktop_set_scroll_interleave_hook_for_test(
    desktop_scroll_interleave_hook_t hook)
{
	g_scroll_interleave_hook = hook;
}

static void desktop_test_scroll_interleave(desktop_state_t *desktop)
{
	if (g_scroll_interleave_hook && g_framebuffer_present_depth == 0)
		g_scroll_interleave_hook(desktop);
}
#else
static void desktop_test_scroll_interleave(desktop_state_t *desktop)
{
	(void)desktop;
}
#endif

static int desktop_pixel_rect_intersect(gui_pixel_rect_t a,
                                        gui_pixel_rect_t b,
                                        gui_pixel_rect_t *out)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;
	int64_t a_right;
	int64_t a_bottom;
	int64_t b_right;
	int64_t b_bottom;

	if (!out)
		return 0;

	a_right = (int64_t)a.x + (int64_t)a.w;
	a_bottom = (int64_t)a.y + (int64_t)a.h;
	b_right = (int64_t)b.x + (int64_t)b.w;
	b_bottom = (int64_t)b.y + (int64_t)b.h;
	left = a.x > b.x ? a.x : b.x;
	top = a.y > b.y ? a.y : b.y;
	right = a_right < b_right ? a_right : b_right;
	bottom = a_bottom < b_bottom ? a_bottom : b_bottom;
	if (right <= left || bottom <= top)
		return 0;
	if (left < INT_MIN || top < INT_MIN || right > INT_MAX || bottom > INT_MAX)
		return 0;
	out->x = (int)left;
	out->y = (int)top;
	out->w = (int)(right - left);
	out->h = (int)(bottom - top);
	return 1;
}

static int desktop_pixel_rect_empty(const gui_pixel_rect_t *r)
{
	return !r || r->w <= 0 || r->h <= 0;
}

/*
 * Compute the bounding box of two rects. Returns the union rect. If either
 * input is empty, returns the other. If both are empty, returns an empty
 * rect. The union is always a superset of both inputs but may also cover
 * pixels that are in neither when the rects are disjoint — this is fine for
 * presenting dirty pixels via a single memcpy sweep.
 */
static gui_pixel_rect_t desktop_pixel_rect_union(gui_pixel_rect_t a,
                                                 gui_pixel_rect_t b)
{
	gui_pixel_rect_t out = {0, 0, 0, 0};
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	if (desktop_pixel_rect_empty(&a))
		return b;
	if (desktop_pixel_rect_empty(&b))
		return a;

	left = a.x < b.x ? a.x : b.x;
	top = a.y < b.y ? a.y : b.y;
	right = (int64_t)a.x + a.w;
	if ((int64_t)b.x + b.w > right)
		right = (int64_t)b.x + b.w;
	bottom = (int64_t)a.y + a.h;
	if ((int64_t)b.y + b.h > bottom)
		bottom = (int64_t)b.y + b.h;
	if (left < INT_MIN || top < INT_MIN || right > INT_MAX || bottom > INT_MAX)
		return out;
	out.x = (int)left;
	out.y = (int)top;
	out.w = (int)(right - left);
	out.h = (int)(bottom - top);
	return out;
}

/*
 * Flush a rect from the back buffer to the visible framebuffer. No-op when
 * direct-draw mode is active (no back buffer attached). Callers must hold
 * the framebuffer present critical section when coordinating multiple
 * flushes into a single visually-atomic update.
 */
static void desktop_framebuffer_flush_rect(desktop_state_t *desktop,
                                           gui_pixel_rect_t rect)
{
	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return;
	if (rect.w <= 0 || rect.h <= 0)
		return;
	if (desktop->framebuffer_batch_depth > 0) {
		desktop->framebuffer_batch_rect =
		    desktop_pixel_rect_union(desktop->framebuffer_batch_rect, rect);
		return;
	}
	framebuffer_present_rect(
	    desktop->framebuffer, rect.x, rect.y, rect.w, rect.h);
}

void desktop_begin_console_batch(desktop_state_t *desktop)
{
	if (!desktop)
		return;

	if (desktop->framebuffer_batch_depth == 0) {
		desktop->framebuffer_batch_rect.x = 0;
		desktop->framebuffer_batch_rect.y = 0;
		desktop->framebuffer_batch_rect.w = 0;
		desktop->framebuffer_batch_rect.h = 0;
		desktop->framebuffer_batch_flags = 0;
		desktop->framebuffer_batch_present_active = 0;
		if (desktop->framebuffer_enabled && desktop->framebuffer) {
			desktop->framebuffer_batch_flags =
			    desktop_framebuffer_present_begin();
			desktop->framebuffer_batch_present_active = 1;
		}
	}
	desktop->framebuffer_batch_depth++;
}

void desktop_end_console_batch(desktop_state_t *desktop)
{
	gui_pixel_rect_t rect;
	uint32_t flags;
	int present_active;

	if (!desktop || desktop->framebuffer_batch_depth <= 0)
		return;

	desktop->framebuffer_batch_depth--;
	if (desktop->framebuffer_batch_depth > 0)
		return;

	rect = desktop->framebuffer_batch_rect;
	flags = desktop->framebuffer_batch_flags;
	present_active = desktop->framebuffer_batch_present_active;
	desktop->framebuffer_batch_rect.x = 0;
	desktop->framebuffer_batch_rect.y = 0;
	desktop->framebuffer_batch_rect.w = 0;
	desktop->framebuffer_batch_rect.h = 0;
	desktop->framebuffer_batch_flags = 0;
	desktop->framebuffer_batch_present_active = 0;

	if (desktop->framebuffer_enabled && desktop->framebuffer &&
	    !desktop_pixel_rect_empty(&rect)) {
		framebuffer_present_rect(
		    desktop->framebuffer, rect.x, rect.y, rect.w, rect.h);
	}
	if (present_active)
		desktop_framebuffer_present_end(flags);
}

static void desktop_pixel_fill_rect(const framebuffer_info_t *fb,
                                    const gui_pixel_rect_t *clip,
                                    int x,
                                    int y,
                                    int w,
                                    int h,
                                    uint32_t color)
{
	gui_pixel_rect_t rect;
	gui_pixel_rect_t clipped;

	if (!fb || !clip || w <= 0 || h <= 0)
		return;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	if (!desktop_pixel_rect_intersect(rect, *clip, &clipped))
		return;
	framebuffer_fill_rect(
	    fb, clipped.x, clipped.y, clipped.w, clipped.h, color);
}

static void desktop_pixel_draw_outline(const framebuffer_info_t *fb,
                                       const gui_pixel_rect_t *clip,
                                       int x,
                                       int y,
                                       int w,
                                       int h,
                                       uint32_t color)
{
	if (!fb || !clip || w <= 0 || h <= 0)
		return;
	desktop_pixel_fill_rect(fb, clip, x, y, w, 1, color);
	desktop_pixel_fill_rect(fb, clip, x, y + h - 1, w, 1, color);
	desktop_pixel_fill_rect(fb, clip, x, y, 1, h, color);
	desktop_pixel_fill_rect(fb, clip, x + w - 1, y, 1, h, color);
}

static int
desktop_point_in_pixel_rect(int x, int y, const gui_pixel_rect_t *rect)
{
	if (!rect || rect->w <= 0 || rect->h <= 0)
		return 0;
	return x >= rect->x && x < rect->x + rect->w && y >= rect->y &&
	       y < rect->y + rect->h;
}

static desktop_app_kind_t desktop_launcher_app_from_index(int index)
{
	switch (index) {
	case 0:
		return DESKTOP_APP_SHELL;
	case 1:
		return DESKTOP_APP_FILES;
	case 2:
		return DESKTOP_APP_PROCESSES;
	case 3:
		return DESKTOP_APP_HELP;
	default:
		return DESKTOP_APP_NONE;
	}
}

static int desktop_point_in_launcher(
    desktop_state_t *desktop, int cell_x, int cell_y, int pixel_x, int pixel_y)
{
	if (!desktop || !desktop->launcher_open)
		return 0;

	if (desktop->framebuffer_enabled && desktop->framebuffer)
		return desktop_point_in_pixel_rect(
		    pixel_x, pixel_y, &desktop->launcher_pixel_rect);

	return cell_x >= desktop->launcher_rect.x &&
	       cell_x < desktop->launcher_rect.x + desktop->launcher_rect.w &&
	       cell_y >= desktop->launcher_rect.y &&
	       cell_y < desktop->launcher_rect.y + desktop->launcher_rect.h;
}

static desktop_app_kind_t desktop_launcher_app_at(
    desktop_state_t *desktop, int cell_x, int cell_y, int pixel_x, int pixel_y)
{
	int index;

	if (!desktop_point_in_launcher(desktop, cell_x, cell_y, pixel_x, pixel_y))
		return DESKTOP_APP_NONE;

	if (desktop->framebuffer_enabled && desktop->framebuffer) {
		index =
		    (pixel_y - (desktop->launcher_pixel_rect.y + 12)) / (int)GUI_FONT_H;
	} else {
		index = cell_y - desktop->launcher_rect.y - 1;
	}

	return desktop_launcher_app_from_index(index);
}

static int desktop_point_in_titlebar(const desktop_window_t *win, int x, int y)
{
	gui_pixel_rect_t titlebar;

	if (!win)
		return 0;
	titlebar = win->rect;
	titlebar.h = DESKTOP_WINDOW_CHROME_H;
	return desktop_point_in_pixel_rect(x, y, &titlebar);
}

static int
desktop_point_in_close_button(const desktop_window_t *win, int x, int y)
{
	gui_pixel_rect_t close_button;

	if (!win)
		return 0;
	close_button.x = win->rect.x + win->rect.w - DESKTOP_WINDOW_CLOSE_BUTTON_W -
	                 DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN;
	close_button.y = win->rect.y + DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN;
	close_button.w = DESKTOP_WINDOW_CLOSE_BUTTON_W;
	close_button.h = DESKTOP_WINDOW_CLOSE_BUTTON_H;
	return desktop_point_in_pixel_rect(x, y, &close_button);
}

static gui_pixel_theme_t desktop_pixel_theme(const framebuffer_info_t *fb)
{
	gui_pixel_theme_t theme;

	k_memset(&theme, 0, sizeof(theme));
	theme.desktop_bg = framebuffer_pack_rgb(fb, 0x24, 0x3a, 0x3f);
	theme.taskbar_bg = framebuffer_pack_rgb(fb, 0xd9, 0xde, 0xd5);
	theme.taskbar_fg = framebuffer_pack_rgb(fb, 0x11, 0x18, 0x1c);
	theme.window_bg = framebuffer_pack_rgb(fb, 0x2f, 0x49, 0x50);
	theme.window_border = framebuffer_pack_rgb(fb, 0xf2, 0xc9, 0x4c);
	theme.title_bg = framebuffer_pack_rgb(fb, 0x9a, 0x35, 0x4f);
	theme.title_fg = framebuffer_pack_rgb(fb, 0xff, 0xf7, 0xe8);
	theme.terminal_bg = framebuffer_pack_rgb(fb, 0x08, 0x10, 0x18);
	theme.terminal_fg = framebuffer_pack_rgb(fb, 0xf6, 0xf1, 0xde);
	theme.terminal_dim = framebuffer_pack_rgb(fb, 0x84, 0x93, 0x9a);
	theme.terminal_cursor = framebuffer_pack_rgb(fb, 0x67, 0xc5, 0x8f);
	theme.scrollbar_track = framebuffer_pack_rgb(fb, 0x18, 0x26, 0x2c);
	theme.scrollbar_thumb = framebuffer_pack_rgb(fb, 0x6f, 0xd6, 0xd2);
	return theme;
}

static void desktop_layout(desktop_state_t *desktop)
{
	int margin_x = desktop->display->cols >= 100 ? 10 : 6;
	int margin_top = desktop->display->rows >= 40 ? 4 : 3;
	int taskbar_gap = desktop->display->rows >= 40 ? 8 : 7;
	int shell_w;
	int shell_h;

	desktop->taskbar.x = 0;
	desktop->taskbar.y = desktop->display->rows - 1;
	desktop->taskbar.w = desktop->display->cols;
	desktop->taskbar.h = 1;

	desktop->launcher_rect.x = 1;
	desktop->launcher_rect.y = desktop->display->rows - 7;
	desktop->launcher_rect.w = 18;
	desktop->launcher_rect.h = 6;

	shell_w = desktop->display->cols - margin_x * 2;
	shell_h = desktop->display->rows - margin_top - taskbar_gap;
	if (shell_w < 48 && desktop->display->cols >= 60)
		shell_w = 48;
	if (shell_h < 15 && desktop->display->rows >= 25)
		shell_h = 15;

	desktop->shell_rect.x = margin_x;
	desktop->shell_rect.y = margin_top;
	desktop->shell_rect.w = shell_w;
	desktop->shell_rect.h = shell_h;

	desktop->shell_content.x = desktop->shell_rect.x + 1;
	desktop->shell_content.y = desktop->shell_rect.y + 1;
	desktop->shell_content.w = desktop->shell_rect.w - 2;
	desktop->shell_content.h = desktop->shell_rect.h - 2;

	desktop->taskbar_pixel_rect.x = 0;
	desktop->taskbar_pixel_rect.y = desktop->taskbar.y * (int)GUI_FONT_H;
	desktop->taskbar_pixel_rect.w = desktop->display->cols * (int)GUI_FONT_W;
	desktop->taskbar_pixel_rect.h = (int)GUI_FONT_H;

	desktop->launcher_pixel_rect.x = desktop->launcher_rect.x * (int)GUI_FONT_W;
	desktop->launcher_pixel_rect.y = desktop->launcher_rect.y * (int)GUI_FONT_H;
	desktop->launcher_pixel_rect.w = desktop->launcher_rect.w * (int)GUI_FONT_W;
	desktop->launcher_pixel_rect.h = desktop->launcher_rect.h * (int)GUI_FONT_H;

	desktop->window_pixel_rect.x = desktop->shell_rect.x * (int)GUI_FONT_W;
	desktop->window_pixel_rect.y = desktop->shell_rect.y * (int)GUI_FONT_H;
	desktop->window_pixel_rect.w = desktop->shell_rect.w * (int)GUI_FONT_W;
	desktop->window_pixel_rect.h = desktop->shell_rect.h * (int)GUI_FONT_H;

	desktop->shell_pixel_rect.x = desktop->window_pixel_rect.x + 8;
	desktop->shell_pixel_rect.y = desktop->window_pixel_rect.y + 24;
	desktop->shell_pixel_rect.w =
	    DESKTOP_TERMINAL_PADDING_X + desktop->shell_content.w * (int)GUI_FONT_W;
	desktop->shell_pixel_rect.h =
	    DESKTOP_TERMINAL_PADDING_Y + desktop->shell_content.h * (int)GUI_FONT_H;
	if (desktop->shell_pixel_rect.w < 0)
		desktop->shell_pixel_rect.w = 0;
	if (desktop->shell_pixel_rect.h < 0)
		desktop->shell_pixel_rect.h = 0;
	if (desktop->shell_pixel_rect.w >
	    desktop->window_pixel_rect.w - DESKTOP_TERMINAL_PADDING_X)
		desktop->shell_pixel_rect.w =
		    desktop->window_pixel_rect.w - DESKTOP_TERMINAL_PADDING_X;
	if (desktop->shell_pixel_rect.h > desktop->window_pixel_rect.h - 24)
		desktop->shell_pixel_rect.h = desktop->window_pixel_rect.h - 24;
	if (desktop->shell_pixel_rect.w < 0)
		desktop->shell_pixel_rect.w = 0;
	if (desktop->shell_pixel_rect.h < 0)
		desktop->shell_pixel_rect.h = 0;
}

static const char *desktop_app_title(desktop_app_kind_t app)
{
	switch (app) {
	case DESKTOP_APP_SHELL:
		return "Shell";
	case DESKTOP_APP_FILES:
		return "Files";
	case DESKTOP_APP_PROCESSES:
		return "Processes";
	case DESKTOP_APP_HELP:
		return "Help";
	default:
		return "Window";
	}
}

static int desktop_app_kind_is_valid(desktop_app_kind_t app)
{
	switch (app) {
	case DESKTOP_APP_SHELL:
	case DESKTOP_APP_FILES:
	case DESKTOP_APP_PROCESSES:
	case DESKTOP_APP_HELP:
		return 1;
	case DESKTOP_APP_NONE:
	default:
		return 0;
	}
}

static desktop_window_t *desktop_find_window(desktop_state_t *desktop,
                                             int window_id)
{
	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (desktop->windows[i].open && desktop->windows[i].id == window_id)
			return &desktop->windows[i];
	}
	return 0;
}

static desktop_window_t *desktop_find_app_window(desktop_state_t *desktop,
                                                 desktop_app_kind_t app)
{
	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (desktop->windows[i].open && desktop->windows[i].app == app)
			return &desktop->windows[i];
	}
	return 0;
}

static desktop_window_t *
desktop_find_topmost_open_window(desktop_state_t *desktop)
{
	desktop_window_t *best = 0;

	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		desktop_window_t *win = &desktop->windows[i];

		if (!win->open)
			continue;
		if (!best || win->z > best->z)
			best = win;
	}
	return best;
}

static desktop_window_t *desktop_next_window_by_z(desktop_state_t *desktop,
                                                  int min_z)
{
	desktop_window_t *best = 0;

	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		desktop_window_t *win = &desktop->windows[i];
		if (!win->open || win->z <= min_z)
			continue;
		if (!best || win->z < best->z)
			best = win;
	}
	return best;
}

/*
 * Return 1 if any open, opaque window fully contains the clip. When true,
 * the desktop background and anything underneath the window are invisible
 * inside the clip and the caller can skip filling them entirely. This is
 * safe because all desktop windows are opaque — no alpha blending.
 */
static int desktop_clip_fully_covered_by_window(const desktop_state_t *desktop,
                                                const gui_pixel_rect_t *clip)
{
	int clip_right;
	int clip_bottom;

	if (!desktop || !clip || clip->w <= 0 || clip->h <= 0)
		return 0;
	clip_right = clip->x + clip->w;
	clip_bottom = clip->y + clip->h;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		const desktop_window_t *win = &desktop->windows[i];

		if (!win->open || win->rect.w <= 0 || win->rect.h <= 0)
			continue;
		if (win->rect.x <= clip->x && win->rect.y <= clip->y &&
		    win->rect.x + win->rect.w >= clip_right &&
		    win->rect.y + win->rect.h >= clip_bottom)
			return 1;
	}
	return 0;
}

static int desktop_shell_content_has_overlap_above(desktop_state_t *desktop,
                                                   const gui_pixel_rect_t *rect)
{
	desktop_window_t *shell_win;

	if (!desktop || !rect)
		return 0;

	shell_win = desktop_find_app_window(desktop, DESKTOP_APP_SHELL);
	if (!shell_win || !shell_win->open)
		return 0;

	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		desktop_window_t *win = &desktop->windows[i];
		gui_pixel_rect_t overlap;

		if (!win->open || win->id == shell_win->id || win->z <= shell_win->z)
			continue;
		if (desktop_pixel_rect_intersect(win->rect, *rect, &overlap))
			return 1;
	}
	return 0;
}

static desktop_window_t *
desktop_top_window_at(desktop_state_t *desktop, int x, int y)
{
	desktop_window_t *best = 0;

	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		desktop_window_t *win = &desktop->windows[i];

		if (!win->open)
			continue;
		if (!desktop_point_in_pixel_rect(x, y, &win->rect))
			continue;
		if (!best || win->z > best->z)
			best = win;
	}
	return best;
}

static desktop_window_t *
desktop_taskbar_window_at(desktop_state_t *desktop, int cell_x, int cell_y)
{
	int slot = 0;

	if (!desktop || cell_y != desktop->taskbar.y)
		return 0;
	if (cell_x < 8)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (!desktop->windows[i].open)
			continue;
		if (cell_x >= 8 + slot * 10 && cell_x < 17 + slot * 10)
			return &desktop->windows[i];
		slot++;
	}
	return 0;
}

static void desktop_default_window_rect(desktop_state_t *desktop,
                                        desktop_app_kind_t app,
                                        gui_pixel_rect_t *out)
{
	int desktop_w = 80 * (int)GUI_FONT_W;
	int desktop_h = 25 * (int)GUI_FONT_H;
	int taskbar_h = (int)GUI_FONT_H;

	if (!out)
		return;

	if (desktop && desktop->framebuffer) {
		desktop_w = (int)desktop->framebuffer->width;
		desktop_h = (int)desktop->framebuffer->height;
	} else if (desktop && desktop->display) {
		desktop_w = desktop->display->cols * (int)GUI_FONT_W;
		desktop_h = desktop->display->rows * (int)GUI_FONT_H;
	}

	out->w = desktop_w - 160;
	out->h = desktop_h - taskbar_h - 96;
	if (out->w < 280)
		out->w = 280;
	if (out->h < 180)
		out->h = 180;
	if (app == DESKTOP_APP_SHELL) {
		out->x = 48;
		out->y = 48;
	} else if (app == DESKTOP_APP_FILES) {
		out->x = 72;
		out->y = 64;
		out->w = 360;
		out->h = 260;
	} else if (app == DESKTOP_APP_PROCESSES) {
		out->x = 160;
		out->y = 96;
		out->w = 420;
		out->h = 240;
	} else {
		out->x = 220;
		out->y = 128;
		out->w = 360;
		out->h = 240;
	}
}

static void desktop_update_window_content_rect(desktop_window_t *win)
{
	if (!win)
		return;
	win->content_rect.x = win->rect.x + 8;
	win->content_rect.y = win->rect.y + 24;
	win->content_rect.w = win->rect.w - 16;
	win->content_rect.h = win->rect.h - 32;
	if (win->content_rect.w < 0)
		win->content_rect.w = 0;
	if (win->content_rect.h < 0)
		win->content_rect.h = 0;
}

static void desktop_clamp_window_rect(desktop_state_t *desktop,
                                      gui_pixel_rect_t *rect)
{
	int max_w;
	int max_h;

	if (!desktop || !rect)
		return;

	if (desktop->framebuffer_enabled && desktop->framebuffer) {
		max_w = (int)desktop->framebuffer->width;
		max_h = (int)desktop->framebuffer->height;
	} else if (desktop->display) {
		max_w = desktop->display->cols * (int)GUI_FONT_W;
		max_h = desktop->display->rows * (int)GUI_FONT_H;
	} else {
		return;
	}

	if (rect->w < 0)
		rect->w = 0;
	if (rect->h < 0)
		rect->h = 0;
	if (rect->w > max_w)
		rect->w = max_w;
	if (rect->h > max_h)
		rect->h = max_h;
	if (rect->x < 0)
		rect->x = 0;
	if (rect->y < 0)
		rect->y = 0;
	if (rect->x + rect->w > max_w)
		rect->x = max_w - rect->w;
	if (rect->y + rect->h > max_h)
		rect->y = max_h - rect->h;
	if (rect->x < 0)
		rect->x = 0;
	if (rect->y < 0)
		rect->y = 0;
}

static void desktop_sync_shell_geometry(desktop_state_t *desktop,
                                        const desktop_window_t *win)
{
	int shell_w;
	int shell_h;
	int content_w;
	int content_h;

	if (!desktop || !win || win->app != DESKTOP_APP_SHELL)
		return;

	shell_w = desktop->shell_rect.w;
	shell_h = desktop->shell_rect.h;
	content_w = desktop->shell_content.w;
	content_h = desktop->shell_content.h;
	desktop->shell_rect.x = win->rect.x / (int)GUI_FONT_W;
	desktop->shell_rect.y = win->rect.y / (int)GUI_FONT_H;
	if (shell_w > 0)
		desktop->shell_rect.w = shell_w;
	if (shell_h > 0)
		desktop->shell_rect.h = shell_h;

	desktop->shell_content.x = desktop->shell_rect.x + 1;
	desktop->shell_content.y = desktop->shell_rect.y + 1;
	if (content_w > 0)
		desktop->shell_content.w = content_w;
	if (content_h > 0)
		desktop->shell_content.h = content_h;

	desktop->window_pixel_rect = win->rect;
	desktop->shell_pixel_rect.x = win->rect.x + 8;
	desktop->shell_pixel_rect.y = win->rect.y + DESKTOP_WINDOW_CHROME_H + 4;
	desktop->shell_pixel_rect.w =
	    DESKTOP_TERMINAL_PADDING_X + content_w * (int)GUI_FONT_W;
	desktop->shell_pixel_rect.h =
	    DESKTOP_TERMINAL_PADDING_Y + content_h * (int)GUI_FONT_H;
	if (desktop->shell_pixel_rect.w < 0)
		desktop->shell_pixel_rect.w = 0;
	if (desktop->shell_pixel_rect.h < 0)
		desktop->shell_pixel_rect.h = 0;
	gui_terminal_set_pixel_rect(&desktop->shell_terminal,
	                            desktop->shell_pixel_rect,
	                            DESKTOP_TERMINAL_PADDING_X,
	                            DESKTOP_TERMINAL_PADDING_Y);
}

static void desktop_dirty_include(gui_rect_t *dirty, int x, int y, int w, int h)
{
	int right;
	int bottom;
	int dirty_right;
	int dirty_bottom;

	if (!dirty || w <= 0 || h <= 0)
		return;

	if (dirty->w <= 0 || dirty->h <= 0) {
		dirty->x = x;
		dirty->y = y;
		dirty->w = w;
		dirty->h = h;
		return;
	}

	right = x + w;
	bottom = y + h;
	dirty_right = dirty->x + dirty->w;
	dirty_bottom = dirty->y + dirty->h;

	if (x < dirty->x)
		dirty->x = x;
	if (y < dirty->y)
		dirty->y = y;
	if (right > dirty_right)
		dirty_right = right;
	if (bottom > dirty_bottom)
		dirty_bottom = bottom;

	dirty->w = dirty_right - dirty->x;
	dirty->h = dirty_bottom - dirty->y;
}

static void
desktop_shell_clear_line(desktop_state_t *desktop, int row, gui_rect_t *dirty)
{
	int base;

	if (!desktop->shell_cells || row < 0 || row >= desktop->shell_cells_h)
		return;

	desktop_dirty_include(dirty, 0, row, desktop->shell_cells_w, 1);

	base = row * desktop->shell_cells_w;
	for (int col = 0; col < desktop->shell_cells_w; col++) {
		gui_cell_t *cell = &desktop->shell_cells[base + col];
		cell->ch = ' ';
		cell->attr = desktop->display->default_attr;
	}
}

static void desktop_shell_scroll_up(desktop_state_t *desktop, gui_rect_t *dirty)
{
	int bytes;

	if (!desktop->shell_cells || desktop->shell_cells_w <= 0 ||
	    desktop->shell_cells_h <= 0)
		return;

	desktop_dirty_include(
	    dirty, 0, 0, desktop->shell_cells_w, desktop->shell_cells_h);
	bytes = desktop->shell_cells_w * (int)sizeof(gui_cell_t);
	for (int row = 1; row < desktop->shell_cells_h; row++) {
		k_memmove(&desktop->shell_cells[(row - 1) * desktop->shell_cells_w],
		          &desktop->shell_cells[row * desktop->shell_cells_w],
		          (uint32_t)bytes);
	}
	desktop_shell_clear_line(desktop, desktop->shell_cells_h - 1, dirty);
	if (desktop->shell_cursor_y > 0)
		desktop->shell_cursor_y--;
	if (desktop->shell_cursor_y >= desktop->shell_cells_h)
		desktop->shell_cursor_y = desktop->shell_cells_h - 1;
}

static void desktop_shell_ensure_cursor(desktop_state_t *desktop)
{
	if (desktop->shell_cursor_x < 0)
		desktop->shell_cursor_x = 0;
	if (desktop->shell_cursor_y < 0)
		desktop->shell_cursor_y = 0;
	if (desktop->shell_cursor_x >= desktop->shell_cells_w)
		desktop->shell_cursor_x = desktop->shell_cells_w - 1;
	if (desktop->shell_cursor_y >= desktop->shell_cells_h)
		desktop->shell_cursor_y = desktop->shell_cells_h - 1;
}

static void desktop_shell_newline(desktop_state_t *desktop, gui_rect_t *dirty)
{
	desktop->shell_cursor_x = 0;
	desktop->shell_wrap_pending = 0;
	desktop->shell_cursor_y++;
	if (desktop->shell_cursor_y >= desktop->shell_cells_h) {
		desktop_shell_scroll_up(desktop, dirty);
		desktop->shell_cursor_y = desktop->shell_cells_h - 1;
	}
}

static void
desktop_shell_write_cell(desktop_state_t *desktop, char c, gui_rect_t *dirty)
{
	int x;
	int y;
	gui_cell_t *cell;

	if (!desktop->shell_cells || desktop->shell_cells_w <= 0 ||
	    desktop->shell_cells_h <= 0)
		return;

	if (desktop->shell_wrap_pending)
		desktop_shell_newline(desktop, dirty);

	desktop_shell_ensure_cursor(desktop);

	x = desktop->shell_cursor_x;
	y = desktop->shell_cursor_y;
	desktop_dirty_include(dirty, x, y, 1, 1);
	cell = &desktop->shell_cells[y * desktop->shell_cells_w + x];
	cell->ch = c;
	cell->attr = desktop->shell_attr;
	if (desktop->shell_cursor_x == desktop->shell_cells_w - 1) {
		desktop->shell_wrap_pending = 1;
	} else {
		desktop->shell_cursor_x++;
		desktop->shell_wrap_pending = 0;
	}
}

static void desktop_shell_apply_ansi(desktop_state_t *desktop, int code)
{
	if (code == 0)
		desktop->shell_attr = desktop->display->default_attr;
	if (code == 31)
		desktop->shell_attr = 0x0c;
	if (code == 32)
		desktop->shell_attr = 0x0a;
	if (code == 33)
		desktop->shell_attr = 0x0e;
	if (code == 36)
		desktop->shell_attr = 0x0b;
}

static void
desktop_shell_apply_char(desktop_state_t *desktop, char c, gui_rect_t *dirty)
{
	if (c == '\x1b') {
		desktop->shell_ansi_state = 1;
		desktop->shell_ansi_val = 0;
		return;
	}

	if (desktop->shell_ansi_state == 1) {
		desktop->shell_ansi_state = (c == '[') ? 2 : 0;
		return;
	}

	if (desktop->shell_ansi_state == 2) {
		if (c >= '0' && c <= '9') {
			desktop->shell_ansi_val =
			    (desktop->shell_ansi_val * 10) + (c - '0');
			return;
		}
		if (c == 'm') {
			desktop_shell_apply_ansi(desktop, desktop->shell_ansi_val);
			desktop->shell_ansi_state = 0;
			return;
		}
		desktop->shell_ansi_state = 0;
		return;
	}

	if (c == '\r') {
		desktop->shell_cursor_x = 0;
		desktop->shell_wrap_pending = 0;
		return;
	}

	if (c == '\n') {
		desktop_shell_newline(desktop, dirty);
		return;
	}

	if (c == '\b') {
		if (desktop->shell_wrap_pending) {
			desktop->shell_wrap_pending = 0;
			return;
		}
		if (desktop->shell_cursor_x > 0)
			desktop->shell_cursor_x--;
		return;
	}

	if (c == '\t') {
		int spaces = 4 - (desktop->shell_cursor_x % 4);

		if (spaces == 0)
			spaces = 4;
		while (spaces-- > 0)
			desktop_shell_apply_char(desktop, ' ', dirty);
		return;
	}

	desktop_shell_write_cell(desktop, c, dirty);
}

static void desktop_shell_redraw(desktop_state_t *desktop)
{
	if (!desktop->shell_cells || !desktop->display)
		return;

	for (int row = 0; row < desktop->shell_cells_h; row++) {
		for (int col = 0; col < desktop->shell_cells_w; col++) {
			int dx = desktop->shell_content.x + col;
			int dy = desktop->shell_content.y + row;

			if (dx < 0 || dy < 0 || dx >= desktop->display->cols ||
			    dy >= desktop->display->rows)
				continue;
			desktop->display->cells[dy * desktop->display->cols + dx] =
			    desktop->shell_cells[row * desktop->shell_cells_w + col];
		}
	}
}

static void desktop_shell_redraw_rect(desktop_state_t *desktop,
                                      const gui_rect_t *dirty)
{
	if (!desktop->shell_cells || !desktop->display || !dirty)
		return;

	for (int row = dirty->y; row < dirty->y + dirty->h; row++) {
		for (int col = dirty->x; col < dirty->x + dirty->w; col++) {
			int dx = desktop->shell_content.x + col;
			int dy = desktop->shell_content.y + row;

			if (col < 0 || row < 0 || col >= desktop->shell_cells_w ||
			    row >= desktop->shell_cells_h)
				continue;
			if (dx < 0 || dy < 0 || dx >= desktop->display->cols ||
			    dy >= desktop->display->rows)
				continue;
			desktop->display->cells[dy * desktop->display->cols + dx] =
			    desktop->shell_cells[row * desktop->shell_cells_w + col];
		}
	}
}

static void desktop_sync_legacy_shell_from_terminal(desktop_state_t *desktop)
{
	if (!desktop)
		return;

	desktop->shell_cursor_x = gui_terminal_cursor_x(&desktop->shell_terminal);
	desktop->shell_cursor_y = gui_terminal_cursor_y(&desktop->shell_terminal);
	desktop->shell_wrap_pending = desktop->shell_terminal.wrap_pending;
	desktop->shell_ansi_state = desktop->shell_terminal.ansi_state;
	desktop->shell_ansi_val = desktop->shell_terminal.ansi_val;
	desktop->shell_attr = desktop->shell_terminal.attr;

	if (!desktop->shell_cells)
		return;
	for (int row = 0; row < desktop->shell_cells_h; row++) {
		for (int col = 0; col < desktop->shell_cells_w; col++) {
			desktop->shell_cells[row * desktop->shell_cells_w + col] =
			    gui_terminal_cell_at(&desktop->shell_terminal, col, row);
		}
	}
}

static void desktop_terminal_redraw_to_cells(desktop_state_t *desktop)
{
	if (!desktop || !desktop->display)
		return;
	desktop_sync_legacy_shell_from_terminal(desktop);
	desktop_shell_redraw(desktop);
}

static void desktop_set_pointer(desktop_state_t *desktop, int x, int y)
{
	if (!desktop || !desktop->display)
		return;

	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= desktop->display->cols)
		x = desktop->display->cols - 1;
	if (y >= desktop->display->rows)
		y = desktop->display->rows - 1;

	desktop->pointer_x = x;
	desktop->pointer_y = y;
	desktop->pointer_pixel_x = x * (int)GUI_FONT_W;
	desktop->pointer_pixel_y = y * (int)GUI_FONT_H;
	desktop->pointer_visible = 1;
}

static int desktop_clamp_pixel_x(desktop_state_t *desktop, int x)
{
	int max_x;

	if (!desktop || !desktop->framebuffer || desktop->framebuffer->width == 0)
		return 0;
	max_x = 0;
	if (desktop->framebuffer->width > DESKTOP_CURSOR_W)
		max_x = (int)desktop->framebuffer->width - DESKTOP_CURSOR_W;
	if (x < 0)
		return 0;
	if (x > max_x)
		return max_x;
	return x;
}

static int desktop_clamp_pixel_y(desktop_state_t *desktop, int y)
{
	int max_y;

	if (!desktop || !desktop->framebuffer || desktop->framebuffer->height == 0)
		return 0;
	max_y = 0;
	if (desktop->framebuffer->height > DESKTOP_CURSOR_H)
		max_y = (int)desktop->framebuffer->height - DESKTOP_CURSOR_H;
	if (y < 0)
		return 0;
	if (y > max_y)
		return max_y;
	return y;
}

static void desktop_draw_pointer(desktop_state_t *desktop)
{
	if (!desktop || !desktop->display || !desktop->pointer_visible)
		return;

	gui_display_draw_text(
	    desktop->display, desktop->pointer_x, desktop->pointer_y, 1, "^", 0x0f);
}

static void desktop_present_cursor_region(desktop_state_t *desktop,
                                          int pixel_x,
                                          int pixel_y)
{
	int cell_x;
	int cell_y;
	int cell_right;
	int cell_bottom;

	if (!desktop || !desktop->display || !desktop->framebuffer)
		return;

	cell_x = pixel_x / (int)GUI_FONT_W;
	cell_y = pixel_y / (int)GUI_FONT_H;
	cell_right = (pixel_x + DESKTOP_CURSOR_W - 1) / (int)GUI_FONT_W;
	cell_bottom = (pixel_y + DESKTOP_CURSOR_H - 1) / (int)GUI_FONT_H;

	gui_display_present_rect_to_framebuffer(desktop->display,
	                                        desktop->framebuffer,
	                                        cell_x,
	                                        cell_y,
	                                        cell_right - cell_x + 1,
	                                        cell_bottom - cell_y + 1);
}

/*
 * Sync the virtual hardware cursor overlay stored on the framebuffer with
 * the desktop's current pointer position and visibility. When a back
 * buffer is attached the overlay is composited on top of the visible
 * framebuffer during framebuffer_present_rect() — it never touches the
 * back buffer, so cursor motion never causes back-buffer repaints.
 */
static void desktop_set_overlay_cursor(desktop_state_t *desktop)
{
	uint32_t fg;
	uint32_t shadow;

	if (!desktop || !desktop->framebuffer)
		return;

	fg = framebuffer_pack_rgb(desktop->framebuffer, 255, 255, 255);
	shadow = framebuffer_pack_rgb(desktop->framebuffer, 0, 0, 0);
	framebuffer_set_cursor(desktop->framebuffer,
	                       desktop->pointer_pixel_x,
	                       desktop->pointer_pixel_y,
	                       fg,
	                       shadow,
	                       desktop->pointer_visible);
}

/*
 * Make the cursor visible at its current position. With a back buffer we
 * simply keep the overlay sprite in sync — it will be composited onto the
 * visible framebuffer during the next framebuffer_present_rect() call. In
 * direct-draw mode (no back buffer) we fall back to drawing the cursor
 * straight into the visible framebuffer, preserving legacy behaviour.
 */
static void desktop_draw_framebuffer_pointer(desktop_state_t *desktop)
{
	uint32_t fg;
	uint32_t shadow;

	desktop_set_overlay_cursor(desktop);

	if (!desktop || !desktop->framebuffer || !desktop->pointer_visible)
		return;
	if (framebuffer_has_back_buffer(desktop->framebuffer))
		return;

	fg = framebuffer_pack_rgb(desktop->framebuffer, 255, 255, 255);
	shadow = framebuffer_pack_rgb(desktop->framebuffer, 0, 0, 0);
	framebuffer_draw_cursor(desktop->framebuffer,
	                        desktop->pointer_pixel_x,
	                        desktop->pointer_pixel_y,
	                        fg,
	                        shadow);
}

static void desktop_sync_shell_framebuffer_rect(desktop_state_t *desktop,
                                                const desktop_window_t *win)
{
	if (!desktop || !win || win->app != DESKTOP_APP_SHELL)
		return;

	desktop->shell_pixel_rect.x = win->content_rect.x;
	desktop->shell_pixel_rect.y = win->content_rect.y;
	desktop->shell_pixel_rect.w =
	    DESKTOP_TERMINAL_PADDING_X +
	    desktop->shell_terminal.cols * (int)GUI_FONT_W;
	desktop->shell_pixel_rect.h =
	    DESKTOP_TERMINAL_PADDING_Y +
	    desktop->shell_terminal.rows * (int)GUI_FONT_H;
	if (desktop->shell_pixel_rect.w < 0)
		desktop->shell_pixel_rect.w = 0;
	if (desktop->shell_pixel_rect.h < 0)
		desktop->shell_pixel_rect.h = 0;
	gui_terminal_set_pixel_rect(&desktop->shell_terminal,
	                            desktop->shell_pixel_rect,
	                            DESKTOP_TERMINAL_PADDING_X,
	                            DESKTOP_TERMINAL_PADDING_Y);
}

static void desktop_render_framebuffer_window(desktop_state_t *desktop,
                                              const gui_pixel_rect_t *clip,
                                              const gui_pixel_theme_t *theme,
                                              const desktop_window_t *win)
{
	const framebuffer_info_t *fb;
	gui_pixel_surface_t surface;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer ||
	    !clip || !theme || !win || !win->open)
		return;

	fb = desktop->framebuffer;
	desktop_pixel_fill_rect(fb,
	                        clip,
	                        win->rect.x,
	                        win->rect.y,
	                        win->rect.w,
	                        win->rect.h,
	                        theme->window_bg);
	desktop_pixel_fill_rect(fb,
	                        clip,
	                        win->rect.x,
	                        win->rect.y,
	                        win->rect.w,
	                        DESKTOP_WINDOW_CHROME_H,
	                        theme->title_bg);
	framebuffer_draw_text_clipped(fb,
	                              clip,
	                              win->rect.x + 16,
	                              win->rect.y + 2,
	                              desktop_app_title(win->app),
	                              theme->title_fg,
	                              theme->title_bg);
	framebuffer_draw_text_clipped(
	    fb,
	    clip,
	    win->rect.x + win->rect.w - DESKTOP_WINDOW_CLOSE_BUTTON_W -
	        DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN,
	    win->rect.y + DESKTOP_WINDOW_CLOSE_BUTTON_MARGIN,
	    "x",
	    theme->title_fg,
	    theme->title_bg);

	surface.fb = fb;
	surface.clip = *clip;
	if (win->app == DESKTOP_APP_SHELL) {
		desktop_sync_shell_framebuffer_rect(desktop, win);
		gui_terminal_render(&desktop->shell_terminal, &surface, theme, 1);
	} else {
		desktop_app_render(
		    &desktop->app_state, win->app, &surface, theme, &win->content_rect);
	}
	desktop_pixel_draw_outline(fb,
	                           clip,
	                           win->rect.x,
	                           win->rect.y,
	                           win->rect.w,
	                           win->rect.h,
	                           theme->window_border);
}

static void desktop_render_framebuffer_region(desktop_state_t *desktop,
                                              const gui_pixel_rect_t *clip)
{
	const framebuffer_info_t *fb;
	gui_pixel_theme_t theme;
	desktop_window_t *win;
	int min_z;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer ||
	    !clip)
		return;

	fb = desktop->framebuffer;
	theme = desktop_pixel_theme(fb);
	/*
     * Skip the desktop-wide background fill when an opaque window already
     * covers every pixel of the clip. The window will repaint itself a few
     * lines down and would have overwritten every pixel we just wrote.
     * This is the single largest fill in the region path, so dropping it
     * for occluded clips removes most of the per-drag pixel churn.
     */
	if (!desktop_clip_fully_covered_by_window(desktop, clip))
		desktop_pixel_fill_rect(
		    fb, clip, 0, 0, (int)fb->width, (int)fb->height, theme.desktop_bg);
	desktop_pixel_fill_rect(fb,
	                        clip,
	                        desktop->taskbar_pixel_rect.x,
	                        desktop->taskbar_pixel_rect.y,
	                        desktop->taskbar_pixel_rect.w,
	                        desktop->taskbar_pixel_rect.h,
	                        theme.taskbar_bg);
	framebuffer_draw_text_clipped(fb,
	                              clip,
	                              desktop->taskbar_pixel_rect.x + 16,
	                              desktop->taskbar_pixel_rect.y,
	                              "Menu",
	                              theme.taskbar_fg,
	                              theme.taskbar_bg);
	{
		int task_x = desktop->taskbar_pixel_rect.x + 72;

		for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
			if (!desktop->windows[i].open)
				continue;
			framebuffer_draw_text_clipped(fb,
			                              clip,
			                              task_x,
			                              desktop->taskbar_pixel_rect.y,
			                              desktop->windows[i].title,
			                              theme.taskbar_fg,
			                              theme.taskbar_bg);
			task_x += 80;
		}
	}

	min_z = INT_MIN;
	while ((win = desktop_next_window_by_z(desktop, min_z)) != 0) {
		desktop_render_framebuffer_window(desktop, clip, &theme, win);
		min_z = win->z;
	}

	if (desktop->launcher_open) {
		gui_pixel_rect_t launcher = desktop->launcher_pixel_rect;

		desktop_pixel_fill_rect(fb,
		                        clip,
		                        launcher.x,
		                        launcher.y,
		                        launcher.w,
		                        launcher.h,
		                        theme.taskbar_bg);
		desktop_pixel_draw_outline(fb,
		                           clip,
		                           launcher.x,
		                           launcher.y,
		                           launcher.w,
		                           launcher.h,
		                           theme.window_border);
		framebuffer_draw_text_clipped(fb,
		                              clip,
		                              launcher.x + 16,
		                              launcher.y + 12,
		                              "1 Shell",
		                              theme.taskbar_fg,
		                              theme.taskbar_bg);
		framebuffer_draw_text_clipped(fb,
		                              clip,
		                              launcher.x + 16,
		                              launcher.y + 28,
		                              "2 Files",
		                              theme.taskbar_fg,
		                              theme.taskbar_bg);
		framebuffer_draw_text_clipped(fb,
		                              clip,
		                              launcher.x + 16,
		                              launcher.y + 44,
		                              "3 Processes",
		                              theme.taskbar_fg,
		                              theme.taskbar_bg);
		framebuffer_draw_text_clipped(fb,
		                              clip,
		                              launcher.x + 16,
		                              launcher.y + 60,
		                              "4 Help",
		                              theme.taskbar_fg,
		                              theme.taskbar_bg);
	}
}

static void desktop_render_framebuffer(desktop_state_t *desktop)
{
	gui_pixel_rect_t clip;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return;

	clip.x = 0;
	clip.y = 0;
	clip.w = (int)desktop->framebuffer->width;
	clip.h = (int)desktop->framebuffer->height;
	desktop_render_framebuffer_region(desktop, &clip);
	desktop_draw_framebuffer_pointer(desktop);
	desktop_framebuffer_flush_rect(desktop, clip);
}

static void desktop_render_framebuffer_terminal(desktop_state_t *desktop)
{
	desktop_window_t *shell_win;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return;

	shell_win = desktop_find_app_window(desktop, DESKTOP_APP_SHELL);
	if (!shell_win || !shell_win->open)
		return;

	desktop_sync_shell_framebuffer_rect(desktop, shell_win);
	if (desktop->shell_pixel_rect.w <= 0 || desktop->shell_pixel_rect.h <= 0)
		return;

	desktop_render_framebuffer_region(desktop, &desktop->shell_pixel_rect);
	desktop_draw_framebuffer_pointer(desktop);
	desktop_framebuffer_flush_rect(desktop, desktop->shell_pixel_rect);
}

static void desktop_terminal_dirty_include_cell(desktop_state_t *desktop,
                                                gui_rect_t *dirty,
                                                int col,
                                                int row)
{
	int64_t x;
	int64_t y;

	if (!desktop || !dirty)
		return;
	if (col < 0 || row < 0 || col >= desktop->shell_terminal.cols ||
	    row >= desktop->shell_terminal.rows)
		return;

	x = (int64_t)desktop->shell_pixel_rect.x +
	    (int64_t)desktop->shell_terminal.padding_x +
	    (int64_t)col * (int64_t)GUI_FONT_W;
	y = (int64_t)desktop->shell_pixel_rect.y +
	    (int64_t)desktop->shell_terminal.padding_y +
	    (int64_t)row * (int64_t)GUI_FONT_H;
	if (x < INT_MIN || y < INT_MIN ||
	    x > (int64_t)INT_MAX - (int64_t)GUI_FONT_W ||
	    y > (int64_t)INT_MAX - (int64_t)GUI_FONT_H)
		return;

	desktop_dirty_include(
	    dirty, (int)x, (int)y, (int)GUI_FONT_W, (int)GUI_FONT_H);
}

static int desktop_render_framebuffer_write_dirty(desktop_state_t *desktop,
                                                  const char *buf,
                                                  uint32_t len,
                                                  int before_cursor_x,
                                                  int before_cursor_y,
                                                  int before_wrap_pending,
                                                  int before_ansi_state,
                                                  int before_history_count,
                                                  int before_view_top)
{
	gui_rect_t dirty = {0, 0, 0, 0};
	gui_pixel_rect_t clip;
	unsigned char ch;
	int after_cursor_x;
	int after_cursor_y;
	int x;
	int y;
	int wrap;
	int ansi_state;
	int ansi_val;
	uint32_t i;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return 0;
	if (!buf || len == 0)
		return 0;
	if (before_view_top != 0 ||
	    gui_terminal_visible_view_top(&desktop->shell_terminal) != 0)
		return 0;
	if (before_history_count !=
	    gui_terminal_history_count(&desktop->shell_terminal))
		return 0;
	if (before_ansi_state != 0)
		return 0;

	after_cursor_x = gui_terminal_cursor_x(&desktop->shell_terminal);
	after_cursor_y = gui_terminal_cursor_y(&desktop->shell_terminal);
	x = before_cursor_x;
	y = before_cursor_y;
	wrap = before_wrap_pending;
	ansi_state = 0;
	ansi_val = 0;

	desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
	for (i = 0; i < len; i++) {
		ch = (unsigned char)buf[i];

		if (ansi_state == 1) {
			if (ch != '[')
				return 0;
			ansi_state = 2;
			continue;
		}

		if (ansi_state == 2) {
			if (ch >= '0' && ch <= '9') {
				if (ansi_val >= 100) {
					ansi_val = 999;
				} else {
					ansi_val = (ansi_val * 10) + (ch - '0');
					if (ansi_val > 999)
						ansi_val = 999;
				}
				continue;
			}
			if (ch != 'm')
				return 0;
			ansi_state = 0;
			ansi_val = 0;
			continue;
		}

		if (ch == '\x1b') {
			ansi_state = 1;
			ansi_val = 0;
			continue;
		}

		if (ch == 0x7fu)
			return 0;

		if (ch == '\r') {
			desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
			x = 0;
			wrap = 0;
			desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
			continue;
		}

		if (ch == '\n') {
			desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
			x = 0;
			y++;
			wrap = 0;
			if (y >= desktop->shell_terminal.rows)
				return 0;
			desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
			continue;
		}

		if (ch == '\b') {
			desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
			if (wrap) {
				wrap = 0;
			} else if (x > 0) {
				x--;
			}
			desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
			continue;
		}

		if (ch < ' ' || ch == '\t')
			return 0;

		if (wrap) {
			x = 0;
			y++;
			wrap = 0;
			if (y >= desktop->shell_terminal.rows)
				return 0;
		}

		if (x < 0 || y < 0 || x >= desktop->shell_terminal.cols ||
		    y >= desktop->shell_terminal.rows)
			return 0;
		desktop_terminal_dirty_include_cell(desktop, &dirty, x, y);
		if (x == desktop->shell_terminal.cols - 1) {
			wrap = 1;
		} else {
			x++;
			wrap = 0;
		}
	}

	if (ansi_state != 0 || desktop->shell_terminal.ansi_state != 0)
		return 0;
	if (x != after_cursor_x || y != after_cursor_y ||
	    wrap != desktop->shell_terminal.wrap_pending)
		return 0;

	desktop_terminal_dirty_include_cell(
	    desktop, &dirty, after_cursor_x, after_cursor_y);
	if (dirty.w <= 0 || dirty.h <= 0)
		return 0;

	clip.x = dirty.x;
	clip.y = dirty.y;
	clip.w = dirty.w;
	clip.h = dirty.h;
	desktop_render_framebuffer_region(desktop, &clip);
	desktop_draw_framebuffer_pointer(desktop);
	desktop_framebuffer_flush_rect(desktop, clip);
	return 1;
}

static int desktop_terminal_content_pixel_rect(desktop_state_t *desktop,
                                               gui_pixel_rect_t *rect)
{
	int64_t x;
	int64_t y;
	int64_t w;
	int64_t h;

	if (!desktop || !rect)
		return 0;
	if (desktop->shell_terminal.cols <= 0 || desktop->shell_terminal.rows <= 0)
		return 0;

	x = (int64_t)desktop->shell_pixel_rect.x +
	    (int64_t)desktop->shell_terminal.padding_x;
	y = (int64_t)desktop->shell_pixel_rect.y +
	    (int64_t)desktop->shell_terminal.padding_y;
	w = (int64_t)desktop->shell_terminal.cols * (int64_t)GUI_FONT_W;
	h = (int64_t)desktop->shell_terminal.rows * (int64_t)GUI_FONT_H;
	if (x < INT_MIN || y < INT_MIN || w <= 0 || h <= 0 ||
	    x > (int64_t)INT_MAX - w || y > (int64_t)INT_MAX - h || w > INT_MAX ||
	    h > INT_MAX)
		return 0;

	rect->x = (int)x;
	rect->y = (int)y;
	rect->w = (int)w;
	rect->h = (int)h;
	return 1;
}

static int desktop_framebuffer_scroll_rect_up(const framebuffer_info_t *fb,
                                              const gui_pixel_rect_t *rect,
                                              int pixels,
                                              uint32_t fill)
{
	uintptr_t base;
	uint32_t row_pitch;
	uint32_t row_bytes;

	if (!fb || !rect || pixels <= 0 || rect->w <= 0 || rect->h <= 0)
		return 0;
	base = framebuffer_draw_address(fb);
	row_pitch = framebuffer_draw_pitch(fb);
	if (base == 0 || row_pitch == 0)
		return 0;
	if (rect->x < 0 || rect->y < 0 || rect->x > (int)fb->width ||
	    rect->y > (int)fb->height || rect->w > (int)fb->width - rect->x ||
	    rect->h > (int)fb->height - rect->y)
		return 0;
	if ((uint32_t)rect->w > UINT32_MAX / sizeof(uint32_t))
		return 0;

	if (pixels >= rect->h) {
		framebuffer_fill_rect(fb, rect->x, rect->y, rect->w, rect->h, fill);
		return 1;
	}

	row_bytes = (uint32_t)rect->w * (uint32_t)sizeof(uint32_t);
	for (int row = 0; row < rect->h - pixels; row++) {
		uintptr_t dst = base + (uintptr_t)(rect->y + row) * row_pitch +
		                (uintptr_t)rect->x * sizeof(uint32_t);
		uintptr_t src = base + (uintptr_t)(rect->y + row + pixels) * row_pitch +
		                (uintptr_t)rect->x * sizeof(uint32_t);

		k_memmove((void *)dst, (const void *)src, row_bytes);
	}
	framebuffer_fill_rect(
	    fb, rect->x, rect->y + rect->h - pixels, rect->w, pixels, fill);
	return 1;
}

static int desktop_terminal_cell_pixel_rect(desktop_state_t *desktop,
                                            int col,
                                            int row,
                                            gui_pixel_rect_t *rect)
{
	int64_t x;
	int64_t y;

	if (!desktop || !rect)
		return 0;
	if (col < 0 || row < 0 || col >= desktop->shell_terminal.cols ||
	    row >= desktop->shell_terminal.rows)
		return 0;

	x = (int64_t)desktop->shell_pixel_rect.x +
	    (int64_t)desktop->shell_terminal.padding_x +
	    (int64_t)col * (int64_t)GUI_FONT_W;
	y = (int64_t)desktop->shell_pixel_rect.y +
	    (int64_t)desktop->shell_terminal.padding_y +
	    (int64_t)row * (int64_t)GUI_FONT_H;
	if (x < INT_MIN || y < INT_MIN ||
	    x > (int64_t)INT_MAX - (int64_t)GUI_FONT_W ||
	    y > (int64_t)INT_MAX - (int64_t)GUI_FONT_H)
		return 0;

	rect->x = (int)x;
	rect->y = (int)y;
	rect->w = (int)GUI_FONT_W;
	rect->h = (int)GUI_FONT_H;
	return 1;
}

static void
desktop_terminal_dirty_include_row(int *first, int *last, int row, int rows)
{
	if (!first || !last || row < 0 || row >= rows)
		return;
	if (*last < *first) {
		*first = row;
		*last = row;
		return;
	}
	if (row < *first)
		*first = row;
	if (row > *last)
		*last = row;
}

static void desktop_terminal_dirty_shift_up(int *first, int *last)
{
	if (!first || !last || *last < *first)
		return;
	if (*first > 0)
		(*first)--;
	if (*last > 0) {
		(*last)--;
	} else {
		*first = 1;
		*last = 0;
	}
}

static int desktop_render_framebuffer_scroll_dirty(desktop_state_t *desktop,
                                                   const char *buf,
                                                   uint32_t len,
                                                   int before_cursor_x,
                                                   int before_cursor_y,
                                                   int before_wrap_pending,
                                                   int before_ansi_state,
                                                   int before_view_top)
{
	gui_pixel_theme_t theme;
	gui_pixel_rect_t content;
	gui_pixel_rect_t clip;
	unsigned char ch;
	int after_cursor_x;
	int after_cursor_y;
	int ansi_state;
	int ansi_val;
	int scroll_rows;
	int scroll_pixels;
	int wrote_cells;
	int dirty_first;
	int dirty_last;
	int wrap;
	int x;
	int y;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return 0;
	if (!buf || len == 0)
		return 0;
	if (before_view_top != 0 ||
	    gui_terminal_visible_view_top(&desktop->shell_terminal) != 0)
		return 0;
	if (before_ansi_state != 0)
		return 0;

	after_cursor_x = gui_terminal_cursor_x(&desktop->shell_terminal);
	after_cursor_y = gui_terminal_cursor_y(&desktop->shell_terminal);
	x = before_cursor_x;
	y = before_cursor_y;
	wrap = before_wrap_pending;
	ansi_state = 0;
	ansi_val = 0;
	scroll_rows = 0;
	wrote_cells = 0;
	dirty_first = desktop->shell_terminal.rows;
	dirty_last = -1;

	for (uint32_t i = 0; i < len; i++) {
		ch = (unsigned char)buf[i];

		if (ansi_state == 1) {
			if (ch != '[')
				return 0;
			ansi_state = 2;
			continue;
		}

		if (ansi_state == 2) {
			if (ch >= '0' && ch <= '9') {
				if (ansi_val >= 100) {
					ansi_val = 999;
				} else {
					ansi_val = (ansi_val * 10) + (ch - '0');
					if (ansi_val > 999)
						ansi_val = 999;
				}
				continue;
			}
			if (ch != 'm')
				return 0;
			ansi_state = 0;
			ansi_val = 0;
			continue;
		}

		if (ch == '\x1b') {
			ansi_state = 1;
			ansi_val = 0;
			continue;
		}

		if (ch == 0x7fu)
			return 0;

		if (ch == '\r') {
			x = 0;
			wrap = 0;
			continue;
		}

		if (ch == '\n') {
			x = 0;
			y++;
			wrap = 0;
			if (y >= desktop->shell_terminal.rows) {
				scroll_rows++;
				desktop_terminal_dirty_shift_up(&dirty_first, &dirty_last);
				y = desktop->shell_terminal.rows - 1;
				desktop_terminal_dirty_include_row(
				    &dirty_first, &dirty_last, y, desktop->shell_terminal.rows);
			}
			continue;
		}

		if (ch == '\b') {
			if (wrap) {
				wrap = 0;
			} else if (x > 0) {
				x--;
			}
			continue;
		}

		if (ch < ' ' || ch == '\t')
			return 0;

		if (wrap) {
			x = 0;
			y++;
			wrap = 0;
			if (y >= desktop->shell_terminal.rows) {
				scroll_rows++;
				desktop_terminal_dirty_shift_up(&dirty_first, &dirty_last);
				y = desktop->shell_terminal.rows - 1;
				desktop_terminal_dirty_include_row(
				    &dirty_first, &dirty_last, y, desktop->shell_terminal.rows);
			}
		}

		if (x < 0 || y < 0 || x >= desktop->shell_terminal.cols ||
		    y >= desktop->shell_terminal.rows)
			return 0;
		wrote_cells = 1;
		desktop_terminal_dirty_include_row(
		    &dirty_first, &dirty_last, y, desktop->shell_terminal.rows);
		if (x == desktop->shell_terminal.cols - 1) {
			wrap = 1;
		} else {
			x++;
			wrap = 0;
		}
	}

	if (scroll_rows <= 0 || scroll_rows > desktop->shell_terminal.rows)
		return 0;
	if (ansi_state != 0 || desktop->shell_terminal.ansi_state != 0)
		return 0;
	if (x != after_cursor_x || y != after_cursor_y ||
	    wrap != desktop->shell_terminal.wrap_pending)
		return 0;
	if (!desktop_terminal_content_pixel_rect(desktop, &content))
		return 0;
	if (scroll_rows > INT_MAX / (int)GUI_FONT_H)
		return 0;
	if (desktop_shell_content_has_overlap_above(desktop, &content))
		return 0;

	theme = desktop_pixel_theme(desktop->framebuffer);
	scroll_pixels = scroll_rows * (int)GUI_FONT_H;
	if (desktop->pointer_visible) {
		clip.x = desktop->pointer_pixel_x;
		clip.y = desktop->pointer_pixel_y;
		clip.w = DESKTOP_CURSOR_W;
		clip.h = DESKTOP_CURSOR_H;
		desktop_render_framebuffer_region(desktop, &clip);
	}
	desktop_test_scroll_interleave(desktop);

	if (!desktop_framebuffer_scroll_rect_up(
	        desktop->framebuffer, &content, scroll_pixels, theme.terminal_bg))
		return 0;

	clip.x = content.x;
	clip.y = content.y + content.h - scroll_pixels;
	clip.w = content.w;
	clip.h = scroll_pixels;
	desktop_render_framebuffer_region(desktop, &clip);

	if (wrote_cells && dirty_last >= dirty_first) {
		clip.x = content.x;
		clip.y = content.y + dirty_first * (int)GUI_FONT_H;
		clip.w = content.w;
		clip.h = (dirty_last - dirty_first + 1) * (int)GUI_FONT_H;
		desktop_render_framebuffer_region(desktop, &clip);
	}

	if (desktop_terminal_cell_pixel_rect(
	        desktop, before_cursor_x, before_cursor_y - scroll_rows, &clip))
		desktop_render_framebuffer_region(desktop, &clip);

	if (desktop_terminal_cell_pixel_rect(
	        desktop, after_cursor_x, after_cursor_y, &clip))
		desktop_render_framebuffer_region(desktop, &clip);

	desktop_draw_framebuffer_pointer(desktop);
	/*
     * The scroll path updates the content rect, the pointer rect (if the
     * cursor overlaps the shell), and the cursor cells. Flushing the full
     * content rect captures all of them with a single memcpy sweep, which
     * is cheaper than flushing individual sub-rects separately.
     */
	desktop_framebuffer_flush_rect(desktop, content);
	if (desktop->pointer_visible) {
		gui_pixel_rect_t pointer_rect;

		pointer_rect.x = desktop->pointer_pixel_x;
		pointer_rect.y = desktop->pointer_pixel_y;
		pointer_rect.w = DESKTOP_CURSOR_W;
		pointer_rect.h = DESKTOP_CURSOR_H;
		desktop_framebuffer_flush_rect(desktop, pointer_rect);
	}
	return 1;
}

static void desktop_present_framebuffer_pointer_motion(desktop_state_t *desktop,
                                                       int old_pixel_x,
                                                       int old_pixel_y)
{
	gui_pixel_rect_t old_clip;
	gui_pixel_rect_t new_clip;
	gui_pixel_rect_t pointer_union;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return;

	old_clip.x = old_pixel_x;
	old_clip.y = old_pixel_y;
	old_clip.w = DESKTOP_CURSOR_W;
	old_clip.h = DESKTOP_CURSOR_H;
	new_clip.x = desktop->pointer_pixel_x;
	new_clip.y = desktop->pointer_pixel_y;
	new_clip.w = DESKTOP_CURSOR_W;
	new_clip.h = DESKTOP_CURSOR_H;

	if (framebuffer_has_back_buffer(desktop->framebuffer)) {
		/*
         * Back-buffer + overlay-cursor fast path: moving the pointer does
         * NOT dirty the back buffer — the sprite is composited during the
         * back→front flush. We only need to re-present the union of the
         * old and new cursor rects so the old position reveals clean
         * background (no ghost) and the new position shows the freshly
         * composited cursor.
         */
		desktop_set_overlay_cursor(desktop);
		pointer_union = desktop_pixel_rect_union(old_clip, new_clip);
		desktop_framebuffer_flush_rect(desktop, pointer_union);
	} else {
		/*
         * Direct-draw fallback: repaint the old cursor region (erasing
         * the cursor) and the new cursor region (restoring whatever sits
         * under it), then draw the cursor on top of the new region.
         */
		desktop_render_framebuffer_region(desktop, &old_clip);
		desktop_render_framebuffer_region(desktop, &new_clip);
		desktop_draw_framebuffer_pointer(desktop);
	}
}

static int
desktop_present_framebuffer_window_move_blit(desktop_state_t *desktop,
                                             gui_pixel_rect_t old_rect,
                                             gui_pixel_rect_t new_rect)
{
	gui_pixel_rect_t overlap;
	gui_pixel_rect_t exposed_horizontal;
	gui_pixel_rect_t exposed_vertical;
	gui_pixel_rect_t window_union;
	const framebuffer_info_t *fb;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return 0;
	if (old_rect.w <= 0 || old_rect.h <= 0 || new_rect.w != old_rect.w ||
	    new_rect.h != old_rect.h)
		return 0;

	fb = desktop->framebuffer;
	/*
     * Blit-move only pays off when we have a back buffer, because the
     * intermediate memmove would be visible during scan-out otherwise. It
     * also only makes sense when the window content hasn't changed (same
     * w/h, same contents) — we enforce same-size here and trust that drag
     * steps don't reflow text between frames.
     */
	if (!framebuffer_has_back_buffer(fb))
		return 0;

	/*
     * Shift the window pixels from the old position to the new one inside
     * the back buffer. This is one memmove per row — far cheaper than
     * re-rasterising the window chrome, title text, and contents. The
     * handful of overlap pixels (when new_rect overlaps old_rect) are
     * already correct because they came from the same window.
     */
	framebuffer_blit_rect(fb,
	                      old_rect.x,
	                      old_rect.y,
	                      new_rect.x,
	                      new_rect.y,
	                      old_rect.w,
	                      old_rect.h);

	if (desktop_pixel_rect_intersect(old_rect, new_rect, &overlap)) {
		/*
         * Split the "exposed L" behind the old position into two axis-
         * aligned rects so neither touches the window's new position:
         *   horizontal = the old rect above or below the overlap
         *   vertical   = the old rect to the left or right of the overlap
         * We render those regions; desktop_render_framebuffer_region walks
         * all windows, but since the dragged window is at new_rect — which
         * by construction is disjoint from both rects — it won't repaint
         * into the exposed area.
         */
		k_memset(&exposed_horizontal, 0, sizeof(exposed_horizontal));
		k_memset(&exposed_vertical, 0, sizeof(exposed_vertical));
		if (old_rect.y < overlap.y) {
			exposed_horizontal.x = old_rect.x;
			exposed_horizontal.y = old_rect.y;
			exposed_horizontal.w = old_rect.w;
			exposed_horizontal.h = overlap.y - old_rect.y;
		} else if (old_rect.y + old_rect.h > overlap.y + overlap.h) {
			exposed_horizontal.x = old_rect.x;
			exposed_horizontal.y = overlap.y + overlap.h;
			exposed_horizontal.w = old_rect.w;
			exposed_horizontal.h =
			    (old_rect.y + old_rect.h) - (overlap.y + overlap.h);
		}
		if (old_rect.x < overlap.x) {
			exposed_vertical.x = old_rect.x;
			exposed_vertical.y = overlap.y;
			exposed_vertical.w = overlap.x - old_rect.x;
			exposed_vertical.h = overlap.h;
		} else if (old_rect.x + old_rect.w > overlap.x + overlap.w) {
			exposed_vertical.x = overlap.x + overlap.w;
			exposed_vertical.y = overlap.y;
			exposed_vertical.w =
			    (old_rect.x + old_rect.w) - (overlap.x + overlap.w);
			exposed_vertical.h = overlap.h;
		}
		if (exposed_horizontal.w > 0 && exposed_horizontal.h > 0)
			desktop_render_framebuffer_region(desktop, &exposed_horizontal);
		if (exposed_vertical.w > 0 && exposed_vertical.h > 0)
			desktop_render_framebuffer_region(desktop, &exposed_vertical);
	} else {
		/* Disjoint move: the whole old rect needs a full repaint. */
		desktop_render_framebuffer_region(desktop, &old_rect);
	}

	/* Flush the bounding box covering both positions in one memcpy sweep. */
	window_union = desktop_pixel_rect_union(old_rect, new_rect);
	desktop_framebuffer_flush_rect(desktop, window_union);
	return 1;
}

static void desktop_present_framebuffer_window_move(desktop_state_t *desktop,
                                                    gui_pixel_rect_t old_rect,
                                                    gui_pixel_rect_t new_rect,
                                                    int old_pixel_x,
                                                    int old_pixel_y)
{
	gui_pixel_rect_t old_pointer;
	gui_pixel_rect_t new_pointer;
	gui_pixel_rect_t window_union;
	gui_pixel_rect_t pointer_union;
	uint32_t flags;

	if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
		return;

	old_pointer.x = old_pixel_x;
	old_pointer.y = old_pixel_y;
	old_pointer.w = DESKTOP_CURSOR_W;
	old_pointer.h = DESKTOP_CURSOR_H;
	new_pointer.x = desktop->pointer_pixel_x;
	new_pointer.y = desktop->pointer_pixel_y;
	new_pointer.w = DESKTOP_CURSOR_W;
	new_pointer.h = DESKTOP_CURSOR_H;

	flags = desktop_framebuffer_present_begin();

	if (framebuffer_has_back_buffer(desktop->framebuffer)) {
		/*
         * Fast path: blit the window pixels into the new position and
         * redraw only the exposed L-shape behind the old one. If the blit
         * path bails (e.g. mismatched rect sizes) we fall back to a
         * single unioned region render — still better than the four
         * per-frame region renders we used to do.
         */
		if (!desktop_present_framebuffer_window_move_blit(
		        desktop, old_rect, new_rect)) {
			window_union = desktop_pixel_rect_union(old_rect, new_rect);
			desktop_render_framebuffer_region(desktop, &window_union);
			desktop_framebuffer_flush_rect(desktop, window_union);
		}

		/*
         * Cursor handling: with the overlay sprite the cursor isn't drawn
         * into the back buffer, so we don't need a region render for the
         * old or new cursor rect — flushing the union presents clean
         * background at the old spot and composites the cursor at its new
         * position.
         */
		desktop_set_overlay_cursor(desktop);
		pointer_union = desktop_pixel_rect_union(old_pointer, new_pointer);
		desktop_framebuffer_flush_rect(desktop, pointer_union);
	} else {
		/*
         * Direct-draw fallback: preserve the original four-region repaint
         * path exactly so behaviour (and existing tests) remain stable for
         * firmware that gives us a framebuffer too large for our back
         * buffer reservation.
         */
		desktop_render_framebuffer_region(desktop, &old_rect);
		desktop_render_framebuffer_region(desktop, &new_rect);
		desktop_render_framebuffer_region(desktop, &old_pointer);
		desktop_render_framebuffer_region(desktop, &new_pointer);
		desktop_draw_framebuffer_pointer(desktop);
	}

	desktop_framebuffer_present_end(flags);
}

desktop_state_t *desktop_global(void)
{
	return g_desktop;
}

int desktop_is_active(void)
{
	return g_desktop && g_desktop->active && g_desktop->desktop_enabled;
}

void desktop_init(desktop_state_t *desktop, gui_display_t *display)
{
	k_memset(desktop, 0, sizeof(*desktop));
	g_desktop = desktop;
	desktop->display = display;
	desktop->active = 1;
	desktop->desktop_enabled = 1;
	desktop->focus = DESKTOP_FOCUS_TASKBAR;
	desktop->launcher_selection = DESKTOP_APP_SHELL;
	desktop_layout(desktop);
	desktop_set_pointer(desktop, display->cols / 2, display->rows / 2);
	desktop->shell_cells_w = desktop->shell_content.w;
	desktop->shell_cells_h = desktop->shell_content.h;

	if (desktop->shell_cells_w <= 0 || desktop->shell_cells_h <= 0) {
		desktop->active = 0;
		desktop->desktop_enabled = 0;
		return;
	}

	desktop->shell_cells = (gui_cell_t *)kmalloc(
	    (uint32_t)(desktop->shell_cells_w * desktop->shell_cells_h *
	               (int)sizeof(gui_cell_t)));
	if (!desktop->shell_cells) {
		desktop->active = 0;
		desktop->desktop_enabled = 0;
		return;
	}

	if (!gui_terminal_init_alloc(&desktop->shell_terminal,
	                             desktop->shell_content.w,
	                             desktop->shell_content.h,
	                             DESKTOP_TERMINAL_HISTORY_ROWS,
	                             display->default_attr)) {
		kfree(desktop->shell_cells);
		desktop->shell_cells = 0;
		desktop->active = 0;
		desktop->desktop_enabled = 0;
		return;
	}
	gui_terminal_set_pixel_rect(&desktop->shell_terminal,
	                            desktop->shell_pixel_rect,
	                            DESKTOP_TERMINAL_PADDING_X,
	                            DESKTOP_TERMINAL_PADDING_Y);

	desktop->shell_cursor_x = 0;
	desktop->shell_cursor_y = 0;
	desktop->shell_wrap_pending = 0;
	desktop->shell_ansi_state = 0;
	desktop->shell_ansi_val = 0;
	desktop->shell_attr = display->default_attr;
	desktop_apps_init(&desktop->app_state);
	desktop_app_refresh(&desktop->app_state);
	for (int row = 0; row < desktop->shell_cells_h; row++)
		desktop_shell_clear_line(desktop, row, 0);
	desktop->next_window_id = 0;
	desktop->focused_window_id = 0;
	desktop->next_z = 0;
	desktop->dragging_window_id = 0;
}

void desktop_set_presentation_target(desktop_state_t *desktop,
                                     uintptr_t video_address)
{
	if (!desktop)
		return;

	desktop->video_address = video_address;
	desktop->framebuffer_enabled = 0;
	desktop->framebuffer = 0;
}

void desktop_set_framebuffer_target(desktop_state_t *desktop,
                                    framebuffer_info_t *framebuffer)
{
	if (!desktop)
		return;

	desktop->framebuffer = framebuffer;
	desktop->framebuffer_enabled = framebuffer != 0;
}

void desktop_attach_shell_pid(desktop_state_t *desktop, uint32_t pid)
{
	desktop_attach_shell_process(desktop, pid, pid);
}

void desktop_attach_shell_process(desktop_state_t *desktop,
                                  uint32_t pid,
                                  uint32_t pgid)
{
	if (!desktop)
		return;

	desktop->shell_pid = pid;
	desktop->shell_pgid = pgid ? pgid : pid;
}

uint32_t desktop_shell_pid(const desktop_state_t *desktop)
{
	return desktop ? desktop->shell_pid : 0;
}

int desktop_process_owns_shell(desktop_state_t *desktop,
                               uint32_t pid,
                               uint32_t pgid)
{
	if (!desktop || desktop->shell_pid == 0)
		return 0;
	if (pid == desktop->shell_pid)
		return 1;
	if (desktop->shell_pgid != 0 && pgid == desktop->shell_pgid)
		return 1;
	return 0;
}

int desktop_console_mirror_enabled(void)
{
	return !desktop_is_active();
}

int desktop_write_console_output(desktop_state_t *desktop,
                                 const char *buf,
                                 uint32_t len)
{
	int written;
	uint32_t flags;
	int before_cursor_x;
	int before_cursor_y;
	int before_wrap_pending;
	int before_ansi_state;
	int before_history_count;
	int before_view_top;

	if (!desktop || !desktop->active || !desktop->desktop_enabled)
		return 0;
	if (!desktop->shell_window_open)
		return 0;
	if (!buf || len == 0 || !desktop->shell_terminal.live)
		return 0;

	before_cursor_x = gui_terminal_cursor_x(&desktop->shell_terminal);
	before_cursor_y = gui_terminal_cursor_y(&desktop->shell_terminal);
	before_wrap_pending = desktop->shell_terminal.wrap_pending;
	before_ansi_state = desktop->shell_terminal.ansi_state;
	before_history_count = gui_terminal_history_count(&desktop->shell_terminal);
	before_view_top = gui_terminal_visible_view_top(&desktop->shell_terminal);

	written = gui_terminal_write(&desktop->shell_terminal, buf, len);
	desktop_sync_legacy_shell_from_terminal(desktop);
	if (written == 0)
		return 0;

	if (desktop->framebuffer_enabled && desktop->framebuffer) {
		flags = desktop_framebuffer_present_begin();
		if (!desktop_render_framebuffer_write_dirty(desktop,
		                                            buf,
		                                            len,
		                                            before_cursor_x,
		                                            before_cursor_y,
		                                            before_wrap_pending,
		                                            before_ansi_state,
		                                            before_history_count,
		                                            before_view_top)) {
			if (!desktop_render_framebuffer_scroll_dirty(desktop,
			                                             buf,
			                                             len,
			                                             before_cursor_x,
			                                             before_cursor_y,
			                                             before_wrap_pending,
			                                             before_ansi_state,
			                                             before_view_top))
				desktop_render_framebuffer_terminal(desktop);
		}
		desktop_framebuffer_present_end(flags);
	} else {
		desktop_terminal_redraw_to_cells(desktop);
		desktop_render(desktop);
	}
	return written;
}

int desktop_clear_console(desktop_state_t *desktop)
{
	uint32_t flags;

	if (!desktop || !desktop->active || !desktop->desktop_enabled)
		return 0;
	if (!desktop->shell_window_open || !desktop->shell_terminal.live)
		return 0;

	gui_terminal_clear(&desktop->shell_terminal);
	desktop_sync_legacy_shell_from_terminal(desktop);
	if (desktop->framebuffer_enabled && desktop->framebuffer) {
		flags = desktop_framebuffer_present_begin();
		desktop_render_framebuffer_terminal(desktop);
		desktop_framebuffer_present_end(flags);
	} else {
		desktop_render(desktop);
	}
	return 1;
}

int desktop_scroll_console(desktop_state_t *desktop, int rows)
{
	uint32_t flags;

	if (!desktop || !desktop->active || !desktop->desktop_enabled)
		return 0;
	if (!desktop->shell_window_open || !desktop->shell_terminal.live)
		return 0;

	gui_terminal_scroll_view(&desktop->shell_terminal, rows);
	desktop_sync_legacy_shell_from_terminal(desktop);
	if (desktop->framebuffer_enabled && desktop->framebuffer) {
		flags = desktop_framebuffer_present_begin();
		desktop_render_framebuffer_terminal(desktop);
		desktop_framebuffer_present_end(flags);
	} else {
		desktop_render(desktop);
	}
	return 1;
}

int desktop_write_process_output(desktop_state_t *desktop,
                                 uint32_t pid,
                                 uint32_t pgid,
                                 const char *buf,
                                 uint32_t len)
{
	if (!desktop_process_owns_shell(desktop, pid, pgid))
		return 0;
	return desktop_write_console_output(desktop, buf, len);
}

desktop_key_result_t desktop_handle_key(desktop_state_t *desktop, char c)
{
	desktop_window_t *focused_window;
	desktop_app_key_result_t app_key_result;

	if (!desktop || !desktop->active)
		return DESKTOP_KEY_FORWARD;

	if (c == 27) {
		desktop->launcher_open = !desktop->launcher_open;
		desktop->focus = desktop->launcher_open ? DESKTOP_FOCUS_LAUNCHER
		                                        : DESKTOP_FOCUS_SHELL;
		desktop_render(desktop);
		return DESKTOP_KEY_CONSUMED;
	}

	if (desktop->launcher_open && c == '\n') {
		desktop->launcher_open = 0;
		desktop_open_app_window(desktop, desktop->launcher_selection);
		desktop_render(desktop);
		return DESKTOP_KEY_CONSUMED;
	}

	if (desktop->launcher_open && c >= '1' && c <= '4') {
		desktop->launcher_selection = c == '1'   ? DESKTOP_APP_SHELL
		                              : c == '2' ? DESKTOP_APP_FILES
		                              : c == '3' ? DESKTOP_APP_PROCESSES
		                                         : DESKTOP_APP_HELP;
		desktop_render(desktop);
		return DESKTOP_KEY_CONSUMED;
	}

	if (desktop->launcher_open && c == '\t') {
		desktop->focus = DESKTOP_FOCUS_TASKBAR;
		desktop_render(desktop);
		return DESKTOP_KEY_CONSUMED;
	}

	focused_window = desktop_find_window(desktop, desktop->focused_window_id);
	if (desktop->focus == DESKTOP_FOCUS_WINDOW && focused_window &&
	    focused_window->app != DESKTOP_APP_SHELL) {
		app_key_result = desktop_app_handle_key(
		    &desktop->app_state, focused_window->app, (uint32_t)c);
		if (app_key_result == DESKTOP_APP_KEY_CLOSE) {
			desktop_close_window(desktop, focused_window->id);
			desktop_render(desktop);
		} else if (app_key_result == DESKTOP_APP_KEY_HANDLED) {
			desktop_render(desktop);
		}
		return DESKTOP_KEY_CONSUMED;
	}

	return (desktop->focus == DESKTOP_FOCUS_SHELL) ? DESKTOP_KEY_FORWARD
	                                               : DESKTOP_KEY_CONSUMED;
}

static void desktop_launcher_move_selection(desktop_state_t *desktop, int delta)
{
	int index;

	if (!desktop)
		return;

	index = (int)desktop->launcher_selection - (int)DESKTOP_APP_SHELL;
	if (index < 0 || index > 3)
		index = 0;
	index = (index + delta + 4) % 4;
	desktop->launcher_selection = desktop_launcher_app_from_index(index);
}

static int desktop_focused_shell_window(const desktop_state_t *desktop)
{
	int i;

	if (!desktop)
		return 0;

	for (i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		const desktop_window_t *window = &desktop->windows[i];
		if (window->open && window->id == desktop->focused_window_id &&
		    window->app == DESKTOP_APP_SHELL)
			return 1;
	}
	return 0;
}

desktop_key_result_t desktop_handle_key_sequence(desktop_state_t *desktop,
                                                 const char *seq)
{
	if (!seq || !seq[0])
		return DESKTOP_KEY_FORWARD;
	if (!seq[1])
		return desktop_handle_key(desktop, seq[0]);

	if (!desktop || !desktop->active)
		return DESKTOP_KEY_FORWARD;

	if (desktop->launcher_open) {
		if (k_strcmp(seq, "\x1b[A") == 0 || k_strcmp(seq, "\x1b[D") == 0) {
			desktop_launcher_move_selection(desktop, -1);
			desktop_render(desktop);
		} else if (k_strcmp(seq, "\x1b[B") == 0 ||
		           k_strcmp(seq, "\x1b[C") == 0) {
			desktop_launcher_move_selection(desktop, 1);
			desktop_render(desktop);
		}
		return DESKTOP_KEY_CONSUMED;
	}

	if (desktop->focus == DESKTOP_FOCUS_SHELL)
		return DESKTOP_KEY_FORWARD;
	if (desktop->focus == DESKTOP_FOCUS_WINDOW &&
	    desktop_focused_shell_window(desktop))
		return DESKTOP_KEY_FORWARD;
	return DESKTOP_KEY_CONSUMED;
}

void desktop_open_shell_window(desktop_state_t *desktop)
{
	if (!desktop)
		return;
	desktop->shell_window_open = 1;
	desktop_open_app_window(desktop, DESKTOP_APP_SHELL);
}

void desktop_focus_window(desktop_state_t *desktop, int window_id)
{
	desktop_window_t *target;

	if (!desktop)
		return;
	target = desktop_find_window(desktop, window_id);
	if (!target)
		return;
	desktop->focused_window_id = window_id;
	desktop->focus = target->app == DESKTOP_APP_SHELL ? DESKTOP_FOCUS_SHELL
	                                                  : DESKTOP_FOCUS_WINDOW;
	desktop_app_refresh_for_focus(&desktop->app_state, target->app);
	target->z = ++desktop->next_z;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++)
		desktop->windows[i].focused = desktop->windows[i].id == window_id;
}

int desktop_open_app_window(desktop_state_t *desktop, desktop_app_kind_t app)
{
	desktop_window_t *existing;
	desktop_window_t *slot = 0;

	if (!desktop || !desktop_app_kind_is_valid(app))
		return -1;
	existing = desktop_find_app_window(desktop, app);
	if (existing) {
		if (app == DESKTOP_APP_SHELL) {
			existing->rect = desktop->window_pixel_rect;
			desktop_clamp_window_rect(desktop, &existing->rect);
			desktop_update_window_content_rect(existing);
			desktop_sync_shell_geometry(desktop, existing);
			desktop->shell_window_open = 1;
		}
		desktop_focus_window(desktop, existing->id);
		return existing->id;
	}
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (!desktop->windows[i].open) {
			slot = &desktop->windows[i];
			break;
		}
	}
	if (!slot)
		return -1;

	k_memset(slot, 0, sizeof(*slot));
	slot->id = ++desktop->next_window_id;
	slot->open = 1;
	slot->app = app;
	k_strncpy(
	    slot->title, desktop_app_title(app), DESKTOP_WINDOW_TITLE_MAX - 1);
	desktop_default_window_rect(desktop, app, &slot->rect);
	if (app == DESKTOP_APP_SHELL) {
		slot->rect = desktop->window_pixel_rect;
	}
	desktop_clamp_window_rect(desktop, &slot->rect);
	desktop_update_window_content_rect(slot);
	if (app == DESKTOP_APP_SHELL) {
		desktop_sync_shell_geometry(desktop, slot);
		desktop->shell_window_open = 1;
	}
	desktop_focus_window(desktop, slot->id);
	return slot->id;
}

int desktop_close_window(desktop_state_t *desktop, int window_id)
{
	desktop_window_t *win = desktop_find_window(desktop, window_id);
	desktop_window_t *next;

	if (!win)
		return 0;
	if (desktop->dragging_window_id == window_id)
		desktop->dragging_window_id = 0;
	if (win->app == DESKTOP_APP_SHELL)
		desktop->shell_window_open = 0;
	k_memset(win, 0, sizeof(*win));
	if (desktop->focused_window_id != window_id)
		return 1;

	desktop->focused_window_id = 0;
	next = 0;
	if (desktop->shell_window_open)
		next = desktop_find_app_window(desktop, DESKTOP_APP_SHELL);
	if (!next)
		next = desktop_find_topmost_open_window(desktop);
	if (next) {
		desktop->focused_window_id = next->id;
		desktop->focus = next->app == DESKTOP_APP_SHELL ? DESKTOP_FOCUS_SHELL
		                                                : DESKTOP_FOCUS_WINDOW;
		next->focused = 1;
		next->z = ++desktop->next_z;
	} else {
		desktop->focus = DESKTOP_FOCUS_TASKBAR;
	}
	return 1;
}

void desktop_handle_pointer(desktop_state_t *desktop,
                            const desktop_pointer_event_t *ev)
{
	uint32_t flags;
	int old_pixel_x;
	int old_pixel_y;
	int launcher_open;
	int pointer_pixel_x;
	int pointer_pixel_y;
	desktop_window_t *top;
	desktop_window_t *drag;
	desktop_app_kind_t launcher_app;

	if (!desktop || !ev)
		return;

	old_pixel_x = desktop->pointer_pixel_x;
	old_pixel_y = desktop->pointer_pixel_y;
	launcher_open = desktop->launcher_open;
	pointer_pixel_x = ev->pixel_x;
	pointer_pixel_y = ev->pixel_y;
	if (!desktop->framebuffer_enabled && !desktop->framebuffer &&
	    pointer_pixel_x == 0 && pointer_pixel_y == 0 &&
	    (ev->x != 0 || ev->y != 0)) {
		pointer_pixel_x = ev->x * (int)GUI_FONT_W;
		pointer_pixel_y = ev->y * (int)GUI_FONT_H;
	}

	desktop_set_pointer(desktop, ev->x, ev->y);
	if (desktop->framebuffer_enabled && desktop->framebuffer) {
		desktop->pointer_pixel_x =
		    desktop_clamp_pixel_x(desktop, pointer_pixel_x);
		desktop->pointer_pixel_y =
		    desktop_clamp_pixel_y(desktop, pointer_pixel_y);
	}

	if (!ev->left_down) {
		desktop->dragging_window_id = 0;
	} else if (desktop->dragging_window_id != 0) {
		drag = desktop_find_window(desktop, desktop->dragging_window_id);
		if (drag) {
			gui_pixel_rect_t old_rect = drag->rect;

			drag->rect.x = pointer_pixel_x - desktop->drag_offset_x;
			drag->rect.y = pointer_pixel_y - desktop->drag_offset_y;
			desktop_clamp_window_rect(desktop, &drag->rect);
			desktop_update_window_content_rect(drag);
			if (drag->app == DESKTOP_APP_SHELL)
				desktop_sync_shell_geometry(desktop, drag);
			if (desktop->framebuffer_enabled && desktop->framebuffer)
				desktop_present_framebuffer_window_move(
				    desktop, old_rect, drag->rect, old_pixel_x, old_pixel_y);
			else
				desktop_render(desktop);
			return;
		}
		desktop->dragging_window_id = 0;
	}

	if (ev->left_down && desktop->launcher_open) {
		launcher_app = desktop_launcher_app_at(
		    desktop, ev->x, ev->y, pointer_pixel_x, pointer_pixel_y);
		if (launcher_app != DESKTOP_APP_NONE) {
			desktop->launcher_open = 0;
			desktop_open_app_window(desktop, launcher_app);
			desktop_render(desktop);
			return;
		}
		if (desktop_point_in_launcher(
		        desktop, ev->x, ev->y, pointer_pixel_x, pointer_pixel_y)) {
			desktop_render(desktop);
			return;
		}
	}

	top = desktop_top_window_at(desktop, pointer_pixel_x, pointer_pixel_y);
	if (top && ev->left_down) {
		desktop_focus_window(desktop, top->id);
		if (desktop_point_in_close_button(
		        top, pointer_pixel_x, pointer_pixel_y)) {
			desktop_close_window(desktop, top->id);
			desktop_render(desktop);
			return;
		}
		if (desktop_point_in_titlebar(top, pointer_pixel_x, pointer_pixel_y)) {
			desktop->dragging_window_id = top->id;
			desktop->drag_offset_x = pointer_pixel_x - top->rect.x;
			desktop->drag_offset_y = pointer_pixel_y - top->rect.y;
			desktop_render(desktop);
			return;
		}
	}

	if (ev->left_down && ev->x >= 1 && ev->x < 6 &&
	    ev->y == desktop->taskbar.y) {
		desktop->launcher_open = !desktop->launcher_open;
		desktop->focus = desktop->launcher_open ? DESKTOP_FOCUS_LAUNCHER
		                                        : DESKTOP_FOCUS_SHELL;
	}

	if (ev->left_down) {
		desktop_window_t *task_win =
		    desktop_taskbar_window_at(desktop, ev->x, ev->y);

		if (task_win) {
			desktop_focus_window(desktop, task_win->id);
			desktop_render(desktop);
			return;
		}
	}

	if (desktop->framebuffer_enabled && desktop->framebuffer &&
	    launcher_open == desktop->launcher_open) {
		flags = desktop_framebuffer_present_begin();
		desktop_present_framebuffer_pointer_motion(
		    desktop, old_pixel_x, old_pixel_y);
		desktop_framebuffer_present_end(flags);
	} else {
		desktop_render(desktop);
	}
}

void desktop_render(desktop_state_t *desktop)
{
	uint32_t flags;

	if (!desktop || !desktop->display)
		return;

	if (desktop->framebuffer_enabled && desktop->framebuffer) {
		flags = desktop_framebuffer_present_begin();
		desktop_render_framebuffer(desktop);
		desktop_framebuffer_present_end(flags);
		return;
	}

	gui_display_fill_rect(desktop->display,
	                      0,
	                      0,
	                      desktop->display->cols,
	                      desktop->display->rows,
	                      ' ',
	                      DESKTOP_ATTR_BACKGROUND);
	gui_display_fill_rect(desktop->display,
	                      desktop->taskbar.x,
	                      desktop->taskbar.y,
	                      desktop->taskbar.w,
	                      desktop->taskbar.h,
	                      ' ',
	                      DESKTOP_ATTR_TASKBAR);
	gui_display_draw_text(desktop->display,
	                      2,
	                      desktop->taskbar.y,
	                      10,
	                      "Menu",
	                      DESKTOP_ATTR_TITLE);
	{
		int task_x = 8;

		for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
			if (!desktop->windows[i].open)
				continue;
			gui_display_draw_text(desktop->display,
			                      task_x,
			                      desktop->taskbar.y,
			                      9,
			                      desktop->windows[i].title,
			                      DESKTOP_ATTR_TITLE);
			task_x += 10;
		}
	}

	if (desktop->shell_window_open) {
		gui_display_draw_frame(desktop->display,
		                       desktop->shell_rect.x,
		                       desktop->shell_rect.y,
		                       desktop->shell_rect.w,
		                       desktop->shell_rect.h,
		                       DESKTOP_ATTR_WINDOW);
		gui_display_draw_text(desktop->display,
		                      desktop->shell_rect.x + 2,
		                      desktop->shell_rect.y,
		                      desktop->shell_rect.w - 4,
		                      "Shell",
		                      DESKTOP_ATTR_WINDOW);
		gui_display_draw_text(desktop->display,
		                      desktop->shell_rect.x + desktop->shell_rect.w - 3,
		                      desktop->shell_rect.y,
		                      1,
		                      "x",
		                      DESKTOP_ATTR_WINDOW);
	}

	if (desktop->launcher_open) {
		gui_display_draw_frame(desktop->display,
		                       desktop->launcher_rect.x,
		                       desktop->launcher_rect.y,
		                       desktop->launcher_rect.w,
		                       desktop->launcher_rect.h,
		                       DESKTOP_ATTR_LAUNCHER);
		gui_display_draw_text(desktop->display,
		                      desktop->launcher_rect.x + 2,
		                      desktop->launcher_rect.y + 1,
		                      desktop->launcher_rect.w - 4,
		                      "1 Shell",
		                      DESKTOP_ATTR_LAUNCHER);
		gui_display_draw_text(desktop->display,
		                      desktop->launcher_rect.x + 2,
		                      desktop->launcher_rect.y + 2,
		                      desktop->launcher_rect.w - 4,
		                      "2 Files",
		                      DESKTOP_ATTR_LAUNCHER);
		gui_display_draw_text(desktop->display,
		                      desktop->launcher_rect.x + 2,
		                      desktop->launcher_rect.y + 3,
		                      desktop->launcher_rect.w - 4,
		                      "3 Processes",
		                      DESKTOP_ATTR_LAUNCHER);
		gui_display_draw_text(desktop->display,
		                      desktop->launcher_rect.x + 2,
		                      desktop->launcher_rect.y + 4,
		                      desktop->launcher_rect.w - 4,
		                      "4 Help",
		                      DESKTOP_ATTR_LAUNCHER);
	}

	if (desktop->shell_window_open)
		desktop_shell_redraw(desktop);

	if (desktop->video_address) {
		desktop_draw_pointer(desktop);
		gui_display_present_to_vga(desktop->display, desktop->video_address);
	} else {
		desktop_draw_pointer(desktop);
	}
}

#ifdef KTEST_ENABLED
int desktop_window_count_for_test(const desktop_state_t *desktop)
{
	int count = 0;

	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (desktop->windows[i].open)
			count++;
	}
	return count;
}

desktop_app_kind_t desktop_focused_app_for_test(const desktop_state_t *desktop)
{
	if (!desktop)
		return DESKTOP_APP_NONE;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (desktop->windows[i].open &&
		    desktop->windows[i].id == desktop->focused_window_id)
			return desktop->windows[i].app;
	}
	return DESKTOP_APP_NONE;
}

int desktop_window_z_for_test(const desktop_state_t *desktop, int window_id)
{
	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (desktop->windows[i].open && desktop->windows[i].id == window_id)
			return desktop->windows[i].z;
	}
	return 0;
}

const desktop_window_t *desktop_window_for_test(const desktop_state_t *desktop,
                                                int window_id)
{
	if (!desktop)
		return 0;
	for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
		if (desktop->windows[i].open && desktop->windows[i].id == window_id)
			return &desktop->windows[i];
	}
	return 0;
}

int desktop_dragging_window_for_test(const desktop_state_t *desktop)
{
	return desktop ? desktop->dragging_window_id : 0;
}

void desktop_focus_window_for_test(desktop_state_t *desktop, int window_id)
{
	desktop_focus_window(desktop, window_id);
}
#endif

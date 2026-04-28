/*
 * desktop.c - DrunixOS user-space compositor and window server.
 *
 * The desktop owns the framebuffer and taskbar. Application windows are owned
 * by separate client processes through /dev/wm; the desktop maps their
 * surfaces, draws shared chrome, and routes input events back to clients.
 */

#include "cursor_sprite.h"
#include "desktop_font.h"
#include "desktop_window.h"
#include "kbdmap.h"
#include "mman.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "unistd.h"
#include "wm_api.h"

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	uint8_t red_pos;
	uint8_t red_size;
	uint8_t green_pos;
	uint8_t green_size;
	uint8_t blue_pos;
	uint8_t blue_size;
	uint8_t pad[2];
} fbinfo_t;

#define COLOR_DARK_BLUE 0x00071c3au
#define COLOR_TEAL 0x00129896u
#define COLOR_GREEN 0x004da891u
#define COLOR_BLUE 0x000d74b4u
#define COLOR_CLIENT_BG 0x00141414u
#define COLOR_TITLEBAR 0x00374561u
#define COLOR_TITLEBAR_FG 0x00f0f0f0u
#define COLOR_TITLEBAR_BUTTON 0x00475f78u
#define COLOR_TITLEBAR_BUTTON_FG 0x00dce7f2u
#define COLOR_TITLEBAR_CLOSE 0x00644f61u
#define COLOR_CURSOR 0x00ffffffu
#define COLOR_CURSOR_SHADOW 0x00202020u
#define COLOR_TASKBAR 0x0009121fu
#define COLOR_TASKBAR_EDGE 0x001d3147u
#define COLOR_TASKBAR_BUTTON 0x00172436u
#define COLOR_TASKBAR_BUTTON_ACTIVE 0x00294a65u
#define COLOR_TASKBAR_ICON 0x0060d6ffu
#define COLOR_TASKBAR_CLOCK 0x00f4f7fbu

#define TITLE_H 20
#define TASKBAR_H 48
#define TASKBAR_PAD DRUNIX_TASKBAR_PAD
#define TASKBAR_ICON_SIZE DRUNIX_TASKBAR_ICON_SIZE
#define TASKBAR_ICON_GAP DRUNIX_TASKBAR_ICON_GAP
#define POINTER_W DRUNIX_CURSOR_W
#define POINTER_H DRUNIX_CURSOR_H
#define EVT_KEY 'K'
#define EVT_MOUSE 'M'
#define DESKTOP_CLIENT_WINDOWS 8
#define DESKTOP_LAUNCHED_PIDS 16

typedef struct {
	int used;
	uint32_t id;
	int x;
	int y;
	int w;
	int h;
	int minimized;
	int visible;
	char title[DRWIN_MAX_TITLE];
	uint32_t *pixels;
	uint32_t pitch;
	uint32_t mapped_length;
} desktop_client_window_t;

static uint32_t *g_fb;
static uint32_t *g_scene;
static uint32_t *g_wallpaper;
static fbinfo_t g_info;
static uint32_t g_pitch_pixels;
static uint32_t g_fb_bytes;
static int g_clip_enabled;
static drunix_rect_t g_clip_rect;

static int g_wm_fd = -1;
static int g_event_pipe_r = -1;
static desktop_client_window_t g_client_windows[DESKTOP_CLIENT_WINDOWS];
static uint32_t g_focused_window;
static uint32_t g_dragging_window;
static int g_drag_offset_x;
static int g_drag_offset_y;
static int g_launched_pids[DESKTOP_LAUNCHED_PIDS];

static int g_pointer_x;
static int g_pointer_y;
static int g_pointer_old_x;
static int g_pointer_old_y;
static uint8_t g_mouse_buttons;

static uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << g_info.red_pos) | ((uint32_t)g << g_info.green_pos) |
	       ((uint32_t)b << g_info.blue_pos);
}

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

static void put_pixel(uint32_t *target, int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 || x >= (int)g_info.width || y >= (int)g_info.height)
		return;
	if (!point_inside_clip(x, y))
		return;
	target[(uint32_t)y * g_pitch_pixels + (uint32_t)x] = color;
}

static void
fill_rect(uint32_t *target, int x, int y, int w, int h, uint32_t color)
{
	drunix_rect_t rect;
	drunix_rect_t bounds;

	if (w <= 0 || h <= 0)
		return;
	rect = drunix_rect_make(x, y, w, h);
	bounds = g_clip_enabled ? g_clip_rect : screen_rect();
	if (!drunix_rect_clip(rect, bounds, &rect))
		return;

	for (int j = 0; j < rect.h; j++) {
		uint32_t *row =
		    target + (uint32_t)(rect.y + j) * g_pitch_pixels + (uint32_t)rect.x;

		for (int i = 0; i < rect.w; i++)
			row[i] = color;
	}
}

static void copy_rect_from_scene(int x, int y, int w, int h)
{
	if (!g_scene || !g_fb || w <= 0 || h <= 0)
		return;
	for (int j = 0; j < h; j++) {
		int yy = y + j;

		if (yy < 0 || yy >= (int)g_info.height)
			continue;
		for (int i = 0; i < w; i++) {
			int xx = x + i;

			if (xx < 0 || xx >= (int)g_info.width)
				continue;
			g_fb[(uint32_t)yy * g_pitch_pixels + (uint32_t)xx] =
			    g_scene[(uint32_t)yy * g_pitch_pixels + (uint32_t)xx];
		}
	}
}

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

static void
draw_glyph(uint32_t *target, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
	const uint8_t *gly = desktop_font_glyph((unsigned char)ch);

	for (int row = 0; row < 16; row++) {
		uint8_t bits = gly[row];

		for (int col = 0; col < 8; col++) {
			uint32_t color = (bits & (1u << col)) ? fg : bg;

			put_pixel(target, x + col, y + row, color);
		}
	}
}

static void draw_text(
    uint32_t *target, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
	while (*s) {
		draw_glyph(target, x, y, *s, fg, bg);
		x += 8;
		s++;
	}
}

static void render_wallpaper(uint32_t *target)
{
	uint32_t base = pack_rgb(0x07, 0x1c, 0x3a);
	uint32_t teal = pack_rgb(0x12, 0x98, 0x96);
	uint32_t green = pack_rgb(0x4d, 0xa8, 0x91);
	uint32_t blue = pack_rgb(0x0d, 0x74, 0xb4);

	fill_rect(target, 0, 0, (int)g_info.width, (int)g_info.height, base);
	for (int y = 0; y < (int)g_info.height; y++) {
		for (int x = 0; x < (int)g_info.width; x++) {
			int w = (int)g_info.width;
			int h = (int)g_info.height;
			int curve_a = h * 62 / 100 - x / 4 + (x * x) / (w * 5);
			int curve_b = h * 82 / 100 - x / 9 + (x * x) / (w * 9);
			int curve_c = h * 34 / 100 + x / 5;
			int da = y - curve_a;
			int db = y - curve_b;
			int dc = y - curve_c;

			if (da < 0)
				da = -da;
			if (db < 0)
				db = -db;
			if (dc < 0)
				dc = -dc;
			if (da < 14)
				put_pixel(target, x, y, teal);
			else if (db < 10)
				put_pixel(target, x, y, green);
			else if (dc < 8 && x > w / 3)
				put_pixel(target, x, y, blue);
		}
	}
}

static void
draw_title_button(uint32_t *target, int x, int y, uint32_t bg, int close_button)
{
	fill_rect(target,
	          x,
	          y,
	          DRUNIX_WINDOW_CONTROL_SIZE,
	          DRUNIX_WINDOW_CONTROL_SIZE,
	          bg);
	if (close_button) {
		for (int i = 3; i < DRUNIX_WINDOW_CONTROL_SIZE - 3; i++) {
			put_pixel(target, x + i, y + i, COLOR_TITLEBAR_BUTTON_FG);
			put_pixel(target,
			          x + i,
			          y + DRUNIX_WINDOW_CONTROL_SIZE - 1 - i,
			          COLOR_TITLEBAR_BUTTON_FG);
		}
		return;
	}
	fill_rect(target,
	          x + 3,
	          y + DRUNIX_WINDOW_CONTROL_SIZE - 4,
	          DRUNIX_WINDOW_CONTROL_SIZE - 6,
	          2,
	          COLOR_TITLEBAR_BUTTON_FG);
}

static const char *window_title(const desktop_client_window_t *win)
{
	return win && win->title[0] ? win->title : "App";
}

static void render_window_frame(uint32_t *target,
                                const desktop_client_window_t *win,
                                int x,
                                int y,
                                int w,
                                int h)
{
	int control_y;

	fill_rect(target, x, y, w, TITLE_H, COLOR_TITLEBAR);
	draw_text(target, x + 8, y + 2, window_title(win), COLOR_TITLEBAR_FG, COLOR_TITLEBAR);
	control_y = drunix_window_control_y(y, TITLE_H);
	draw_title_button(target,
	                  drunix_window_minimize_button_x(x, w),
	                  control_y,
	                  COLOR_TITLEBAR_BUTTON,
	                  0);
	draw_title_button(target,
	                  drunix_window_close_button_x(x, w),
	                  control_y,
	                  COLOR_TITLEBAR_CLOSE,
	                  1);
	fill_rect(target, x, y + TITLE_H, w, h - TITLE_H, COLOR_CLIENT_BG);
}

static desktop_client_window_t *find_client_window(uint32_t id)
{
	for (int i = 0; i < DESKTOP_CLIENT_WINDOWS; i++) {
		if (g_client_windows[i].used && g_client_windows[i].id == id)
			return &g_client_windows[i];
	}
	return 0;
}

static desktop_client_window_t *alloc_client_window(uint32_t id)
{
	desktop_client_window_t *existing = find_client_window(id);

	if (existing)
		return existing;
	for (int i = 0; i < DESKTOP_CLIENT_WINDOWS; i++) {
		if (!g_client_windows[i].used) {
			memset(&g_client_windows[i], 0, sizeof(g_client_windows[i]));
			g_client_windows[i].used = 1;
			g_client_windows[i].id = id;
			g_client_windows[i].visible = 1;
			return &g_client_windows[i];
		}
	}
	return 0;
}

static unsigned int surface_length(const drwin_surface_info_t *info)
{
	if (!info || info->pitch == 0 || info->height == 0)
		return 0;
	if (info->pitch > ((unsigned int)-1) / info->height)
		return 0;
	return info->pitch * info->height;
}

static void copy_title(char dst[DRWIN_MAX_TITLE], const char *src)
{
	unsigned int i;

	if (!src)
		src = "";
	for (i = 0; i + 1u < DRWIN_MAX_TITLE && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
	while (++i < DRWIN_MAX_TITLE)
		dst[i] = '\0';
}

static void free_client_window(desktop_client_window_t *win)
{
	if (!win)
		return;
	if (win->pixels && win->mapped_length)
		(void)sys_munmap(win->pixels, win->mapped_length);
	if (g_focused_window == win->id)
		g_focused_window = 0;
	if (g_dragging_window == win->id)
		g_dragging_window = 0;
	memset(win, 0, sizeof(*win));
}

static int map_client_surface(desktop_client_window_t *win,
                              const drwin_surface_info_t *surface)
{
	unsigned int length;
	void *pixels;

	if (!win || !surface || surface->width == 0 || surface->height == 0 ||
	    surface->pitch == 0 || surface->bpp != DRWIN_BPP)
		return -1;
	length = surface_length(surface);
	if (length == 0)
		return -1;
	if (win->pixels && win->mapped_length == length &&
	    win->pitch == surface->pitch)
		return 0;
	if (win->pixels && win->mapped_length)
		(void)sys_munmap(win->pixels, win->mapped_length);
	pixels = mmap(0,
	              length,
	              PROT_READ | PROT_WRITE,
	              MAP_SHARED,
	              g_wm_fd,
	              surface->map_offset);
	if (pixels == MAP_FAILED) {
		win->pixels = 0;
		win->mapped_length = 0;
		return -1;
	}
	win->pixels = (uint32_t *)pixels;
	win->mapped_length = length;
	win->pitch = surface->pitch;
	return 0;
}

static drunix_rect_t client_window_rect(const desktop_client_window_t *win)
{
	if (!win || !win->used)
		return drunix_rect_make(0, 0, 0, 0);
	return drunix_rect_make(win->x, win->y, win->w, win->h + TITLE_H);
}

static int client_window_visible(const desktop_client_window_t *win)
{
	return win && win->used && win->visible && !win->minimized;
}

static void render_client_window(uint32_t *target, const desktop_client_window_t *win)
{
	if (!client_window_visible(win))
		return;

	render_window_frame(
	    target, win, win->x, win->y, win->w, win->h + TITLE_H);
	if (!win->pixels || win->pitch == 0)
		return;
	for (int row = 0; row < win->h; row++) {
		int sy = win->y + TITLE_H + row;
		uint32_t *src = (uint32_t *)((uint8_t *)win->pixels +
		                             (uint32_t)row * win->pitch);

		for (int col = 0; col < win->w; col++)
			put_pixel(target, win->x + col, sy, src[col]);
	}
}

static void render_windows(uint32_t *target)
{
	for (int i = 0; i < DESKTOP_CLIENT_WINDOWS; i++) {
		if (g_client_windows[i].id != g_focused_window)
			render_client_window(target, &g_client_windows[i]);
	}
	if (g_focused_window) {
		desktop_client_window_t *focused = find_client_window(g_focused_window);

		render_client_window(target, focused);
	}
}

static int title_matches_app(const char *title, int app)
{
	if (!title)
		return 0;
	if (app == DRUNIX_TASKBAR_APP_TERMINAL)
		return strcmp(title, "Terminal") == 0;
	if (app == DRUNIX_TASKBAR_APP_FILES)
		return strcmp(title, "Files") == 0;
	if (app == DRUNIX_TASKBAR_APP_PROCESSES)
		return strcmp(title, "Processes") == 0;
	if (app == DRUNIX_TASKBAR_APP_HELP || app == DRUNIX_TASKBAR_APP_MENU)
		return strcmp(title, "Help") == 0;
	return 0;
}

static int taskbar_app_active(int app)
{
	for (int i = 0; i < DESKTOP_CLIENT_WINDOWS; i++) {
		desktop_client_window_t *win = &g_client_windows[i];

		if (win->used && title_matches_app(win->title, app))
			return 1;
	}
	return 0;
}

static desktop_client_window_t *taskbar_window_for_app(int app)
{
	desktop_client_window_t *fallback = 0;

	for (int i = 0; i < DESKTOP_CLIENT_WINDOWS; i++) {
		desktop_client_window_t *win = &g_client_windows[i];

		if (!win->used || !title_matches_app(win->title, app))
			continue;
		if (win->id == g_focused_window)
			return win;
		if (!fallback)
			fallback = win;
	}
	return fallback;
}

static int client_window_capacity_available(void)
{
	for (int i = 0; i < DESKTOP_CLIENT_WINDOWS; i++) {
		if (!g_client_windows[i].used)
			return 1;
	}
	return 0;
}

static void draw_taskbar_icon_terminal(uint32_t *target, int x, int y)
{
	fill_rect(target, x + 5, y + 6, 20, 16, COLOR_TASKBAR_ICON);
	fill_rect(target, x + 7, y + 8, 16, 12, COLOR_TASKBAR_BUTTON);
	draw_text(
	    target, x + 9, y + 7, ">", COLOR_TASKBAR_ICON, COLOR_TASKBAR_BUTTON);
	fill_rect(target, x + 17, y + 19, 7, 2, COLOR_TASKBAR_ICON);
}

static void draw_taskbar_icon_file(uint32_t *target, int x, int y)
{
	fill_rect(target, x + 7, y + 8, 17, 16, 0x00d9eef4u);
	fill_rect(target, x + 19, y + 8, 5, 5, 0x008cb8c9u);
	fill_rect(target, x + 10, y + 14, 11, 2, 0x00688fa1u);
	fill_rect(target, x + 10, y + 18, 11, 2, 0x00688fa1u);
}

static void draw_taskbar_icon_gear(uint32_t *target, int x, int y)
{
	int cx = x + TASKBAR_ICON_SIZE / 2;
	int cy = y + TASKBAR_ICON_SIZE / 2;

	fill_rect(target, cx - 2, y + 5, 4, 20, COLOR_TASKBAR_ICON);
	fill_rect(target, x + 5, cy - 2, 20, 4, COLOR_TASKBAR_ICON);
	fill_rect(target, x + 8, y + 8, 14, 14, COLOR_TASKBAR_ICON);
	fill_rect(target, x + 11, y + 11, 8, 8, COLOR_TASKBAR_BUTTON);
}

static void draw_taskbar_icon_browser(uint32_t *target, int x, int y)
{
	fill_rect(target, x + 7, y + 7, 16, 16, 0x0017b6d8u);
	fill_rect(target, x + 10, y + 10, 10, 10, COLOR_TASKBAR_BUTTON);
	fill_rect(target, x + 14, y + 5, 7, 5, 0x0037d29au);
	fill_rect(target, x + 8, y + 19, 13, 4, 0x0057cfffu);
}

static void draw_taskbar_icon_logo(uint32_t *target, int x, int y)
{
	fill_rect(target, x + 7, y + 6, 5, 18, 0x0047d36du);
	fill_rect(target, x + 12, y + 6, 9, 5, 0x0047d36du);
	fill_rect(target, x + 12, y + 14, 8, 4, 0x001e91ffu);
	fill_rect(target, x + 19, y + 18, 4, 6, 0x001e91ffu);
}

static int taskbar_icon_y(void)
{
	return drunix_taskbar_icon_y((int)g_info.height, TASKBAR_H);
}

static void draw_taskbar_button(uint32_t *target,
                                int x,
                                int y,
                                int active,
                                void (*icon)(uint32_t *, int, int))
{
	fill_rect(target,
	          x,
	          y,
	          TASKBAR_ICON_SIZE,
	          TASKBAR_ICON_SIZE,
	          active ? COLOR_TASKBAR_BUTTON_ACTIVE : COLOR_TASKBAR_BUTTON);
	icon(target, x, y);
	if (active)
		fill_rect(target,
		          x + TASKBAR_ICON_SIZE / 2 - 5,
		          y + TASKBAR_ICON_SIZE + 3,
		          10,
		          2,
		          COLOR_TASKBAR_ICON);
}

static void format_clock(char *buf, int bufsz)
{
	sys_timespec_t ts;

	if (bufsz <= 0)
		return;
	if (sys_clock_gettime(0, &ts) != 0) {
		snprintf(buf, (size_t)bufsz, "--:--");
		return;
	}
	int minutes = (int)((ts.tv_sec / 60) % 60);
	int hours = (int)((ts.tv_sec / 3600) % 24);

	snprintf(buf, (size_t)bufsz, "%02d:%02d", hours, minutes);
}

static void render_taskbar(uint32_t *target)
{
	int y = (int)g_info.height - TASKBAR_H;
	int icon_y = taskbar_icon_y();
	int x;
	char clock[8];
	int clock_x;

	if (y < 0)
		return;
	fill_rect(target, 0, y, (int)g_info.width, TASKBAR_H, COLOR_TASKBAR);
	fill_rect(target, 0, y, (int)g_info.width, 1, COLOR_TASKBAR_EDGE);

	x = drunix_taskbar_app_x(DRUNIX_TASKBAR_APP_MENU);
	draw_taskbar_button(
	    target, x, icon_y, taskbar_app_active(DRUNIX_TASKBAR_APP_MENU), draw_taskbar_icon_logo);
	x = drunix_taskbar_app_x(DRUNIX_TASKBAR_APP_TERMINAL);
	draw_taskbar_button(target,
	                    x,
	                    icon_y,
	                    taskbar_app_active(DRUNIX_TASKBAR_APP_TERMINAL),
	                    draw_taskbar_icon_terminal);
	x = drunix_taskbar_app_x(DRUNIX_TASKBAR_APP_FILES);
	draw_taskbar_button(target,
	                    x,
	                    icon_y,
	                    taskbar_app_active(DRUNIX_TASKBAR_APP_FILES),
	                    draw_taskbar_icon_file);
	x = drunix_taskbar_app_x(DRUNIX_TASKBAR_APP_PROCESSES);
	draw_taskbar_button(target,
	                    x,
	                    icon_y,
	                    taskbar_app_active(DRUNIX_TASKBAR_APP_PROCESSES),
	                    draw_taskbar_icon_gear);
	x = drunix_taskbar_app_x(DRUNIX_TASKBAR_APP_HELP);
	draw_taskbar_button(target,
	                    x,
	                    icon_y,
	                    taskbar_app_active(DRUNIX_TASKBAR_APP_HELP),
	                    draw_taskbar_icon_browser);

	format_clock(clock, (int)sizeof(clock));
	clock_x = (int)g_info.width - TASKBAR_PAD - 5 * 8;
	draw_text(target, clock_x, y + 16, clock, COLOR_TASKBAR_CLOCK, COLOR_TASKBAR);
}

static void draw_pointer_sprite(void);
static void render_pointer(void);
static void present_scene(void);

static void compose_scene(void)
{
	if (!g_scene)
		return;
	if (g_wallpaper)
		memcpy(g_scene, g_wallpaper, g_fb_bytes);
	else
		render_wallpaper(g_scene);
	render_windows(g_scene);
	render_taskbar(g_scene);
}

static int compose_scene_rect(drunix_rect_t rect)
{
	drunix_rect_t clipped;

	if (!g_scene)
		return 1;
	if (!drunix_rect_clip(rect, screen_rect(), &clipped))
		return 1;
	if (!g_wallpaper)
		return 0;

	copy_wallpaper_rect_to_scene(clipped);
	begin_clip(clipped);
	render_windows(g_scene);
	render_taskbar(g_scene);
	end_clip();
	return 1;
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

	if (!compose_scene_rect(dirty)) {
		present_scene();
		return;
	}
	copy_scene_rect_to_fb(dirty);
	draw_pointer_sprite();
	g_pointer_old_x = g_pointer_x;
	g_pointer_old_y = g_pointer_y;
}

static void present_scene(void)
{
	compose_scene();
	memcpy(g_fb, g_scene, g_fb_bytes);
	draw_pointer_sprite();
	g_pointer_old_x = g_pointer_x;
	g_pointer_old_y = g_pointer_y;
}

static void draw_pointer_sprite(void)
{
	for (int j = 0; j < POINTER_H; j++) {
		for (int i = 0; i < POINTER_W; i++) {
			int cursor_pixel = drunix_cursor_pixel_at(i, j);

			if (cursor_pixel == DRUNIX_CURSOR_PIXEL_FG)
				put_pixel(g_fb, g_pointer_x + i, g_pointer_y + j, COLOR_CURSOR);
			else if (cursor_pixel == DRUNIX_CURSOR_PIXEL_SHADOW)
				put_pixel(g_fb,
				          g_pointer_x + i,
				          g_pointer_y + j,
				          COLOR_CURSOR_SHADOW);
		}
	}
}

static void render_pointer(void)
{
	if (g_pointer_old_x != g_pointer_x || g_pointer_old_y != g_pointer_y)
		copy_rect_from_scene(
		    g_pointer_old_x, g_pointer_old_y, POINTER_W, POINTER_H);
	draw_pointer_sprite();
	g_pointer_old_x = g_pointer_x;
	g_pointer_old_y = g_pointer_y;
}

static int read_fb_info(void)
{
	int fd;
	int read_total = 0;

	fd = sys_open("/dev/fb0info");
	if (fd < 0)
		return -1;
	while (read_total < (int)sizeof(g_info)) {
		int n = sys_read(
		    fd, (char *)&g_info + read_total, (int)sizeof(g_info) - read_total);

		if (n <= 0)
			break;
		read_total += n;
	}
	sys_close(fd);
	if (read_total != (int)sizeof(g_info))
		return -1;
	if (g_info.width == 0 || g_info.height == 0 || g_info.pitch == 0 ||
	    g_info.bpp != 32)
		return -1;
	g_pitch_pixels = g_info.pitch / 4u;
	return 0;
}

static int fd_has_input(int fd)
{
	sys_pollfd_t pfd;

	pfd.fd = fd;
	pfd.events = SYS_POLLIN;
	pfd.revents = 0;
	return sys_poll(&pfd, 1, 0) > 0 && (pfd.revents & SYS_POLLIN);
}

static void emit_event(int pipe_w, char tag, uint8_t a, uint8_t b, uint8_t c)
{
	uint8_t rec[4];

	rec[0] = (uint8_t)tag;
	rec[1] = a;
	rec[2] = b;
	rec[3] = c;
	sys_fwrite(pipe_w, (const char *)rec, (int)sizeof(rec));
}

static int read_event_record(int fd, uint8_t rec[4])
{
	int got = 0;

	while (got < 4) {
		int n = sys_read(fd, (char *)(rec + got), 4 - got);

		if (n <= 0)
			return -1;
		got += n;
	}
	return 0;
}

static void run_kbd_helper(int pipe_w)
{
	int kbd = sys_open("/dev/kbd");
	struct evt {
		uint32_t sec;
		uint32_t usec;
		uint16_t type;
		uint16_t code;
		int32_t value;
	};
	struct evt batch[16];
	kbdmap_state_t kstate = {0, 0};

	if (kbd < 0)
		sys_exit(1);

	for (;;) {
		int n = sys_read(kbd, (char *)batch, (int)sizeof(batch));
		int records;

		if (n <= 0)
			continue;
		records = n / (int)sizeof(struct evt);
		for (int i = 0; i < records; i++) {
			struct evt *e = &batch[i];
			char cooked[KBDMAP_OUT_MIN];
			int produced;

			if (e->type != 0x01)
				continue;
			produced = kbdmap_translate(
			    &kstate, e->code, e->value, cooked, (int)sizeof(cooked));
			for (int j = 0; j < produced; j++)
				emit_event(pipe_w, EVT_KEY, (uint8_t)cooked[j], 0, 0);
		}
	}
}

static void run_mouse_helper(int pipe_w)
{
	int mfd = sys_open("/dev/mouse");
	struct evt {
		uint32_t sec;
		uint32_t usec;
		uint16_t type;
		uint16_t code;
		int32_t value;
	};
	struct evt batch[16];
	int32_t pending_dx = 0;
	int32_t pending_dy = 0;
	uint8_t pending_buttons = 0;

	if (mfd < 0)
		sys_exit(1);

	for (;;) {
		int n = sys_read(mfd, (char *)batch, (int)sizeof(batch));
		int records;

		if (n <= 0)
			continue;
		records = n / (int)sizeof(struct evt);
		for (int i = 0; i < records; i++) {
			struct evt *e = &batch[i];

			if (e->type == 0x02) {
				if (e->code == 0x00)
					pending_dx += e->value;
				else if (e->code == 0x01)
					pending_dy += e->value;
			} else if (e->type == 0x01) {
				uint8_t mask = 0;

				if (e->code == 0x110)
					mask = 0x01u;
				else if (e->code == 0x111)
					mask = 0x02u;
				else if (e->code == 0x112)
					mask = 0x04u;
				if (e->value)
					pending_buttons |= mask;
				else
					pending_buttons &= (uint8_t)~mask;
			} else if (e->type == 0x00) {
				int8_t cdx;
				int8_t cdy;

				if (pending_dx > 127)
					pending_dx = 127;
				if (pending_dx < -127)
					pending_dx = -127;
				if (pending_dy > 127)
					pending_dy = 127;
				if (pending_dy < -127)
					pending_dy = -127;
				cdx = (int8_t)pending_dx;
				cdy = (int8_t)pending_dy;
				emit_event(pipe_w,
				           EVT_MOUSE,
				           pending_buttons,
				           (uint8_t)cdx,
				           (uint8_t)cdy);
				pending_dx = 0;
				pending_dy = 0;
			}
		}
	}
}

static int spawn_helper_child(void (*fn)(int, int), int arg0, int arg1)
{
	int pid = sys_fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (g_wm_fd > 2)
			sys_close(g_wm_fd);
		if (g_event_pipe_r > 2 && g_event_pipe_r != arg0 &&
		    g_event_pipe_r != arg1)
			sys_close(g_event_pipe_r);
		fn(arg0, arg1);
		sys_exit(0);
	}
	return pid;
}

static void wrap_kbd(int pipe_w, int unused)
{
	(void)unused;
	run_kbd_helper(pipe_w);
}

static void wrap_mouse(int pipe_w, int unused)
{
	(void)unused;
	run_mouse_helper(pipe_w);
}

static int wm_server_connect(void)
{
	drwin_register_server_request_t req;

	g_wm_fd = sys_open_flags("/dev/wm", SYS_O_RDWR, 0);
	if (g_wm_fd < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_REGISTER_SERVER;
	req.magic = DRWIN_SERVER_MAGIC;
	return sys_fwrite(g_wm_fd, (const char *)&req, sizeof(req)) ==
	               (int)sizeof(req)
	           ? 0
	           : -1;
}

static void send_client_window_event(uint32_t window,
                                     uint32_t type,
                                     int x,
                                     int y,
                                     int code,
                                     int value)
{
	drwin_send_event_request_t req;

	if (g_wm_fd < 0 || window == 0)
		return;
	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_SEND_EVENT;
	req.event.type = type;
	req.event.window = (int32_t)window;
	req.event.x = x;
	req.event.y = y;
	req.event.code = code;
	req.event.value = value;
	(void)sys_fwrite(g_wm_fd, (const char *)&req, sizeof(req));
}

static void focus_client_window(uint32_t window)
{
	if (window == g_focused_window)
		return;
	if (g_focused_window)
		send_client_window_event(g_focused_window,
		                         DRWIN_EVENT_FOCUS,
		                         0,
		                         0,
		                         0,
		                         0);
	g_focused_window = window;
	if (g_focused_window)
		send_client_window_event(g_focused_window,
		                         DRWIN_EVENT_FOCUS,
		                         0,
		                         0,
		                         0,
		                         1);
}

static desktop_client_window_t *focused_client_window(void)
{
	desktop_client_window_t *win = find_client_window(g_focused_window);

	return client_window_visible(win) ? win : 0;
}

static void clear_focus_if_hidden(const desktop_client_window_t *win)
{
	if (win && win->id == g_focused_window && !client_window_visible(win))
		focus_client_window(0);
}

static drunix_rect_t client_dirty_rect(const desktop_client_window_t *win,
                                       drwin_rect_t dirty)
{
	if (!win || dirty.w <= 0 || dirty.h <= 0)
		return client_window_rect(win);
	return drunix_rect_make(win->x + dirty.x,
	                        win->y + TITLE_H + dirty.y,
	                        dirty.w,
	                        dirty.h);
}

static void handle_wm_create(const drwin_server_msg_t *msg)
{
	desktop_client_window_t *win = alloc_client_window(msg->window);

	if (!win) {
		send_client_window_event(msg->window,
		                         DRWIN_EVENT_CLOSE,
		                         0,
		                         0,
		                         0,
		                         0);
		return;
	}
	win->x = msg->rect.x;
	win->y = msg->rect.y;
	win->w = msg->rect.w;
	win->h = msg->rect.h;
	win->minimized = 0;
	win->visible = 1;
	copy_title(win->title, msg->title);
	if (map_client_surface(win, &msg->surface) != 0) {
		send_client_window_event(msg->window,
		                         DRWIN_EVENT_CLOSE,
		                         0,
		                         0,
		                         0,
		                         0);
		free_client_window(win);
		return;
	}
	focus_client_window(win->id);
	present_scene();
}

static void handle_wm_destroy(const drwin_server_msg_t *msg)
{
	desktop_client_window_t *win = find_client_window(msg->window);
	drunix_rect_t dirty;

	if (!win)
		return;
	dirty = client_window_rect(win);
	free_client_window(win);
	present_dirty_rect(dirty);
}

static void drain_wm_server_messages(void)
{
	for (;;) {
		drwin_server_msg_t msg;
		int n;
		desktop_client_window_t *win;

		n = sys_read(g_wm_fd, (char *)&msg, (int)sizeof(msg));
		if (n != (int)sizeof(msg))
			break;
		if (msg.size != sizeof(msg))
			continue;

		if (msg.type == DRWIN_MSG_CREATE_WINDOW) {
			handle_wm_create(&msg);
		} else if (msg.type == DRWIN_MSG_DESTROY_WINDOW) {
			handle_wm_destroy(&msg);
		} else if (msg.type == DRWIN_MSG_PRESENT_WINDOW) {
			win = find_client_window(msg.window);
			if (win)
				present_dirty_rect(client_dirty_rect(win, msg.rect));
		} else if (msg.type == DRWIN_MSG_SET_TITLE) {
			win = find_client_window(msg.window);
			if (win) {
				copy_title(win->title, msg.title);
				present_dirty_rect(client_window_rect(win));
			}
		} else if (msg.type == DRWIN_MSG_SHOW_WINDOW) {
			win = find_client_window(msg.window);
			if (win) {
				win->visible = msg.visible ? 1 : 0;
				clear_focus_if_hidden(win);
				present_dirty_rect(client_window_rect(win));
			}
		} else if (msg.type == DRWIN_MSG_SERVER_DISCONNECT) {
			break;
		}

		if (!fd_has_input(g_wm_fd))
			break;
	}
}

static void reap_launched_apps(void);

static int launch_taskbar_app(int app)
{
	const char *path = 0;
	char *argv[2];
	int pid;
	int pid_slot = -1;

	if (!client_window_capacity_available())
		return -1;
	reap_launched_apps();
	for (int i = 0; i < DESKTOP_LAUNCHED_PIDS; i++) {
		if (g_launched_pids[i] <= 0) {
			pid_slot = i;
			break;
		}
	}
	if (pid_slot < 0)
		return -1;

	if (app == DRUNIX_TASKBAR_APP_TERMINAL)
		path = "/bin/terminal";
	else if (app == DRUNIX_TASKBAR_APP_FILES)
		path = "/bin/files";
	else if (app == DRUNIX_TASKBAR_APP_PROCESSES)
		path = "/bin/processes";
	else if (app == DRUNIX_TASKBAR_APP_HELP || app == DRUNIX_TASKBAR_APP_MENU)
		path = "/bin/help";
	if (!path)
		return -1;

	pid = sys_fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (g_wm_fd > 2)
			sys_close(g_wm_fd);
		if (g_event_pipe_r > 2)
			sys_close(g_event_pipe_r);
		argv[0] = (char *)path;
		argv[1] = 0;
		sys_exec(path, argv, 1);
		sys_exit(127);
	}
	g_launched_pids[pid_slot] = pid;
	return 0;
}

static void reap_launched_apps(void)
{
	for (int i = 0; i < DESKTOP_LAUNCHED_PIDS; i++) {
		int pid = g_launched_pids[i];

		if (pid <= 0)
			continue;
		int status = 0;
		int waited = sys_waitpid_status(pid, &status, WNOHANG);

		if (waited == pid || waited < 0)
			g_launched_pids[i] = 0;
	}
}

static int window_hit_test_client(const desktop_client_window_t *win, int px, int py)
{
	if (!client_window_visible(win))
		return DRUNIX_WINDOW_HIT_NONE;
	return drunix_window_hit_test(
	    win->x, win->y, win->w, win->h + TITLE_H, TITLE_H, px, py);
}

static desktop_client_window_t *window_at_pointer(int px, int py)
{
	desktop_client_window_t *focused = find_client_window(g_focused_window);

	if (window_hit_test_client(focused, px, py) != DRUNIX_WINDOW_HIT_NONE)
		return focused;
	for (int i = DESKTOP_CLIENT_WINDOWS - 1; i >= 0; i--) {
		desktop_client_window_t *win = &g_client_windows[i];

		if (win->id == g_focused_window)
			continue;
		if (window_hit_test_client(win, px, py) != DRUNIX_WINDOW_HIT_NONE)
			return win;
	}
	return 0;
}

static void clamp_client_window(desktop_client_window_t *win)
{
	int max_x;
	int max_y;

	if (!win)
		return;
	max_x = (int)g_info.width - win->w;
	max_y = (int)g_info.height - TASKBAR_H - (win->h + TITLE_H);
	if (max_x < 0)
		max_x = 0;
	if (max_y < 0)
		max_y = 0;
	if (win->x < 0)
		win->x = 0;
	if (win->y < 0)
		win->y = 0;
	if (win->x > max_x)
		win->x = max_x;
	if (win->y > max_y)
		win->y = max_y;
}

static void send_pointer_event_to_client(const desktop_client_window_t *win,
                                         uint8_t buttons)
{
	if (!win)
		return;
	send_client_window_event(win->id,
	                         DRWIN_EVENT_MOUSE,
	                         g_pointer_x - win->x,
	                         g_pointer_y - win->y - TITLE_H,
	                         0,
	                         buttons);
}

static int handle_taskbar_app_click(int app)
{
	desktop_client_window_t *win;

	if (app == DRUNIX_TASKBAR_APP_NONE)
		return 0;
	win = taskbar_window_for_app(app);
	if (win) {
		win->minimized = 0;
		win->visible = 1;
		focus_client_window(win->id);
		return 1;
	}
	if (launch_taskbar_app(app) != 0)
		return 0;
	return 1;
}

static void handle_mouse_event(uint8_t buttons, int sdx, int sdy)
{
	uint8_t old_buttons = g_mouse_buttons;
	int old_x = g_pointer_x;
	int old_y = g_pointer_y;
	int full_repaint = 0;
	int dirty_repaint = 0;
	drunix_rect_t dirty_rect = drunix_rect_make(0, 0, 0, 0);
	desktop_client_window_t *dragging;

	g_pointer_x += sdx;
	g_pointer_y += sdy;
	if (g_pointer_x < 0)
		g_pointer_x = 0;
	if (g_pointer_y < 0)
		g_pointer_y = 0;
	if (g_pointer_x > (int)g_info.width - POINTER_W)
		g_pointer_x = (int)g_info.width - POINTER_W;
	if (g_pointer_y > (int)g_info.height - POINTER_H)
		g_pointer_y = (int)g_info.height - POINTER_H;

	dragging = find_client_window(g_dragging_window);
	if (dragging && (old_buttons & 0x01u)) {
		drunix_rect_t old_rect = client_window_rect(dragging);

		dragging->x = g_pointer_x - g_drag_offset_x;
		dragging->y = g_pointer_y - g_drag_offset_y;
		clamp_client_window(dragging);
		dirty_rect = drunix_rect_union(old_rect, client_window_rect(dragging));
		dirty_repaint = 1;
	}
	if (g_dragging_window && !(buttons & 0x01u))
		g_dragging_window = 0;

	if ((buttons & 0x01u) && !(old_buttons & 0x01u)) {
		int taskbar_app = drunix_taskbar_app_at(
		    g_pointer_x, g_pointer_y, (int)g_info.height, TASKBAR_H);

		if (handle_taskbar_app_click(taskbar_app)) {
			full_repaint = 1;
		} else {
			desktop_client_window_t *win =
			    window_at_pointer(g_pointer_x, g_pointer_y);

			if (win) {
				int hit = window_hit_test_client(win, g_pointer_x, g_pointer_y);

				focus_client_window(win->id);
				if (hit == DRUNIX_WINDOW_HIT_CLOSE) {
					send_client_window_event(
					    win->id, DRWIN_EVENT_CLOSE, 0, 0, 0, 0);
					full_repaint = 1;
				} else if (hit == DRUNIX_WINDOW_HIT_MINIMIZE) {
					win->minimized = 1;
					clear_focus_if_hidden(win);
					full_repaint = 1;
				} else if (hit == DRUNIX_WINDOW_HIT_TITLE) {
					g_dragging_window = win->id;
					g_drag_offset_x = g_pointer_x - win->x;
					g_drag_offset_y = g_pointer_y - win->y;
					full_repaint = 1;
				} else if (hit == DRUNIX_WINDOW_HIT_BODY) {
					send_pointer_event_to_client(win, buttons);
					full_repaint = 1;
				}
			}
		}
	} else if (buttons != old_buttons || sdx != 0 || sdy != 0) {
		desktop_client_window_t *target =
		    window_at_pointer(g_pointer_x, g_pointer_y);

		if (target &&
		    window_hit_test_client(target, g_pointer_x, g_pointer_y) ==
		        DRUNIX_WINDOW_HIT_BODY)
			send_pointer_event_to_client(target, buttons);
	}

	g_mouse_buttons = buttons;
	if (full_repaint)
		present_scene();
	else if (dirty_repaint)
		present_dirty_rect(dirty_rect);
	else if (old_x != g_pointer_x || old_y != g_pointer_y)
		render_pointer();
}

static void flush_pending_mouse(drunix_mouse_coalesce_t *pending)
{
	if (!pending || !pending->has_mouse)
		return;
	handle_mouse_event(pending->buttons, pending->dx, pending->dy);
	drunix_mouse_coalesce_init(pending);
}

int main(int argc, char **argv)
{
	int fbfd;
	int evt_fds[2];
	int evt_r;
	int evt_w;

	(void)argc;
	(void)argv;

	if (read_fb_info() != 0) {
		sys_write("desktop: cannot read /dev/fb0info\n");
		return 1;
	}

	fbfd = sys_open_flags("/dev/fb0", SYS_O_RDWR, 0);
	if (fbfd < 0) {
		sys_write("desktop: cannot open /dev/fb0\n");
		return 1;
	}
	g_fb_bytes = g_info.pitch * g_info.height;
	g_fb = (uint32_t *)mmap(
	    0, g_fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (g_fb == MAP_FAILED) {
		sys_write("desktop: framebuffer mmap failed\n");
		return 1;
	}
	sys_close(fbfd);

	g_scene = (uint32_t *)malloc(g_fb_bytes);
	if (!g_scene) {
		sys_write("desktop: scene allocation failed\n");
		return 1;
	}
	g_wallpaper = (uint32_t *)malloc(g_fb_bytes);
	if (g_wallpaper)
		render_wallpaper(g_wallpaper);

	if (sys_display_claim() != 0) {
		sys_write("desktop: cannot claim display\n");
		return 1;
	}
	if (wm_server_connect() != 0) {
		sys_write("desktop: wm server unavailable\n");
		return 1;
	}

	if (sys_pipe(evt_fds) != 0) {
		sys_write("desktop: pipe failed\n");
		return 1;
	}
	evt_r = evt_fds[0];
	evt_w = evt_fds[1];
	g_event_pipe_r = evt_r;
	(void)spawn_helper_child(wrap_kbd, evt_w, 0);
	(void)spawn_helper_child(wrap_mouse, evt_w, 0);
	sys_close(evt_w);

	g_pointer_x = (int)g_info.width / 2;
	g_pointer_y = (int)g_info.height / 2;
	g_pointer_old_x = g_pointer_x;
	g_pointer_old_y = g_pointer_y;

	present_scene();
	(void)launch_taskbar_app(DRUNIX_TASKBAR_APP_TERMINAL);

	for (;;) {
		sys_pollfd_t pfds[2];
		int wm_poll_idx = 0;
		int evt_poll_idx = 1;

		pfds[wm_poll_idx].fd = g_wm_fd;
		pfds[wm_poll_idx].events = SYS_POLLIN;
		pfds[wm_poll_idx].revents = 0;
		pfds[evt_poll_idx].fd = evt_r;
		pfds[evt_poll_idx].events = SYS_POLLIN;
		pfds[evt_poll_idx].revents = 0;

		if (sys_poll(pfds, 2, -1) <= 0) {
			sys_yield();
			continue;
		}

		if (pfds[wm_poll_idx].revents & SYS_POLLIN)
			drain_wm_server_messages();

		if (pfds[evt_poll_idx].revents & SYS_POLLIN) {
			drunix_mouse_coalesce_t pending_mouse;

			drunix_mouse_coalesce_init(&pending_mouse);
			while ((pfds[evt_poll_idx].revents & SYS_POLLIN) ||
			       fd_has_input(evt_r)) {
				uint8_t rec[4];

				pfds[evt_poll_idx].revents = 0;
				if (read_event_record(evt_r, rec) != 0)
					break;
				if (rec[0] == EVT_KEY) {
					desktop_client_window_t *focused =
					    focused_client_window();

					if (focused)
						send_client_window_event(focused->id,
						                         DRWIN_EVENT_KEY,
						                         0,
						                         0,
						                         rec[1],
						                         rec[1]);
				} else if (rec[0] == EVT_MOUSE) {
					int8_t sdx = (int8_t)rec[2];
					int8_t sdy = (int8_t)rec[3];

					if (rec[1] != g_mouse_buttons) {
						flush_pending_mouse(&pending_mouse);
						handle_mouse_event(rec[1], (int)sdx, (int)sdy);
					} else {
						drunix_mouse_coalesce_add(
						    &pending_mouse, rec[1], (int)sdx, (int)sdy);
					}
				}
			}
			flush_pending_mouse(&pending_mouse);
		}
		reap_launched_apps();
	}

	return 0;
}

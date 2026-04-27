/*
 * desktop.c — DrunixOS user-space compositor.
 *
 * Maps /dev/fb0, runs a shell on a freshly-allocated pty pair, and
 * renders the slave-side output into a windowed terminal on top of
 * a procedural / JPEG wallpaper.  Forked helpers stream input through
 * small pipes so the parent can poll for whichever source is ready:
 *
 *   - keyboard helper:  /dev/kbd → 'K' tagged events
 *   - terminal helper:  pty master → byte stream
 *   - mouse helper:     /dev/mouse → 'M' tagged 3-byte packets
 *
 * The parent forwards keystrokes to the pty master, feeds terminal
 * bytes through a minimal ANSI consumer in batches, and tracks the
 * pointer position so the cursor sprite follows the mouse.
 *
 * Framebuffer geometry comes from /dev/fb0info at startup; nothing
 * about the screen is hard-coded.
 */

#include "desktop_font.h"
#include "cursor_sprite.h"
#include "desktop_window.h"
#include "kbdmap.h"
#include "lib/mman.h"
#include "lib/nanojpeg/nanojpeg.h"
#include "lib/stdio.h"
#include "lib/stdlib.h"
#include "lib/string.h"
#include "lib/syscall.h"
#include "lib/unistd.h"

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
#define COLOR_TERM_BG 0x00141414u
#define COLOR_TERM_FG 0x00d0d0d0u
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

#define TERM_COLS 80
#define TERM_ROWS 24
#define TERM_GLYPH_W 8
#define TERM_GLYPH_H 16
#define TERM_PAD 8
#define TITLE_H 20
#define TERM_DEFAULT_X 64
#define TERM_DEFAULT_Y 64

#define TASKBAR_H 48
#define TASKBAR_PAD 14
#define TASKBAR_ICON_SIZE 30
#define TASKBAR_ICON_GAP 10

#define POINTER_W DRUNIX_CURSOR_W
#define POINTER_H DRUNIX_CURSOR_H

#define EVT_KEY 'K'
#define EVT_MOUSE 'M'

static uint32_t *g_fb;
static uint32_t *g_scene;
static uint32_t *g_wallpaper;
static fbinfo_t g_info;
static uint32_t g_pitch_pixels;
static uint32_t g_fb_bytes;

static char g_grid[TERM_ROWS][TERM_COLS];
static int g_cursor_x;
static int g_cursor_y;
static int g_esc_state;

static int g_term_x = TERM_DEFAULT_X;
static int g_term_y = TERM_DEFAULT_Y;
static int g_terminal_minimized;
static int g_terminal_closed;
static int g_dragging_terminal;
static int g_shell_pid = -1;
static int g_term_helper_pid = -1;
static int g_ptmx = -1;
static int g_term_pipe_r = -1;

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

static void put_pixel(uint32_t *target, int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 || x >= (int)g_info.width || y >= (int)g_info.height)
		return;
	target[(uint32_t)y * g_pitch_pixels + (uint32_t)x] = color;
}

static void
fill_rect(uint32_t *target, int x, int y, int w, int h, uint32_t color)
{
	if (w <= 0 || h <= 0)
		return;
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= (int)g_info.height)
			continue;
		uint32_t *row = target + (uint32_t)yy * g_pitch_pixels;
		for (int i = 0; i < w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= (int)g_info.width)
				continue;
			row[xx] = color;
		}
	}
}

static void copy_rect_from_scene(int x, int y, int w, int h)
{
	if (!g_scene)
		return;
	if (w <= 0 || h <= 0)
		return;
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= (int)g_info.height)
			continue;
		uint32_t *src = g_scene + (uint32_t)yy * g_pitch_pixels;
		uint32_t *dst = g_fb + (uint32_t)yy * g_pitch_pixels;
		for (int i = 0; i < w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= (int)g_info.width)
				continue;
			dst[xx] = src[xx];
		}
	}
}

static void
draw_glyph(uint32_t *target, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
	const uint8_t *gly = desktop_font_glyph((unsigned char)ch);

	for (int row = 0; row < TERM_GLYPH_H; row++) {
		uint8_t bits = gly[row];
		for (int col = 0; col < TERM_GLYPH_W; col++) {
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
		x += TERM_GLYPH_W;
		s++;
	}
}

static void render_wallpaper_procedural(uint32_t *target)
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

static int try_load_wallpaper_jpeg(const char *path, uint32_t *target)
{
	int fd;
	int read_total = 0;
	int n;
	int decoded = 0;
	uint8_t *jpeg_buf = 0;
	uint32_t jpeg_cap = 16u * 1024u;
	uint8_t *rgb;
	int src_w;
	int src_h;
	int color;

	fd = sys_open(path);
	if (fd < 0)
		return 0;

	jpeg_buf = (uint8_t *)malloc(jpeg_cap);
	if (!jpeg_buf) {
		sys_close(fd);
		return 0;
	}
	for (;;) {
		if ((uint32_t)read_total + 4096u > jpeg_cap) {
			uint32_t new_cap = jpeg_cap * 2u;
			uint8_t *grown = (uint8_t *)malloc(new_cap);
			if (!grown)
				goto out_free;
			memcpy(grown, jpeg_buf, (size_t)read_total);
			free(jpeg_buf);
			jpeg_buf = grown;
			jpeg_cap = new_cap;
		}
		n = sys_read(fd, (char *)(jpeg_buf + read_total), 4096);
		if (n <= 0)
			break;
		read_total += n;
	}
	sys_close(fd);
	if (read_total < 2)
		goto out_free;

	njInit();
	if (njDecode(jpeg_buf, read_total) != NJ_OK)
		goto out_done;

	src_w = njGetWidth();
	src_h = njGetHeight();
	color = njIsColor();
	rgb = njGetImage();
	if (src_w <= 0 || src_h <= 0 || !rgb)
		goto out_done;

	for (uint32_t y = 0; y < g_info.height; y++) {
		uint32_t sy = (y * (uint32_t)src_h) / g_info.height;
		const uint8_t *srow = rgb + sy * (uint32_t)src_w * (color ? 3u : 1u);
		uint32_t *drow = target + y * g_pitch_pixels;

		for (uint32_t x = 0; x < g_info.width; x++) {
			uint32_t sx = (x * (uint32_t)src_w) / g_info.width;
			uint8_t r;
			uint8_t g;
			uint8_t b;

			if (color) {
				const uint8_t *p = srow + sx * 3u;
				r = p[0];
				g = p[1];
				b = p[2];
			} else {
				r = g = b = srow[sx];
			}
			drow[x] = pack_rgb(r, g, b);
		}
	}
	decoded = 1;

out_done:
	njDone();
out_free:
	free(jpeg_buf);
	return decoded;
}

static void render_wallpaper(uint32_t *target)
{
	if (!try_load_wallpaper_jpeg("/etc/wallpaper.jpg", target))
		render_wallpaper_procedural(target);
}

static void term_clear(void)
{
	for (int r = 0; r < TERM_ROWS; r++)
		for (int c = 0; c < TERM_COLS; c++)
			g_grid[r][c] = ' ';
	g_cursor_x = 0;
	g_cursor_y = 0;
}

static void term_scroll(void)
{
	for (int r = 0; r < TERM_ROWS - 1; r++)
		for (int c = 0; c < TERM_COLS; c++)
			g_grid[r][c] = g_grid[r + 1][c];
	for (int c = 0; c < TERM_COLS; c++)
		g_grid[TERM_ROWS - 1][c] = ' ';
}

static void term_putchar(char ch)
{
	if (g_esc_state == 1) {
		if (ch == '[')
			g_esc_state = 2;
		else
			g_esc_state = 0;
		return;
	}
	if (g_esc_state == 2) {
		if (ch >= '@' && ch <= '~')
			g_esc_state = 0;
		return;
	}

	if (ch == 0x1b) {
		g_esc_state = 1;
		return;
	}
	if (ch == '\n') {
		g_cursor_x = 0;
		g_cursor_y++;
	} else if (ch == '\r') {
		g_cursor_x = 0;
	} else if (ch == '\b' || ch == 0x7f) {
		if (g_cursor_x > 0) {
			g_cursor_x--;
			g_grid[g_cursor_y][g_cursor_x] = ' ';
		}
	} else if (ch == '\t') {
		int next = (g_cursor_x + 8) & ~7;
		if (next > TERM_COLS)
			next = TERM_COLS;
		while (g_cursor_x < next)
			g_grid[g_cursor_y][g_cursor_x++] = ' ';
	} else if ((unsigned char)ch >= 32 && (unsigned char)ch < 127) {
		if (g_cursor_x >= TERM_COLS) {
			g_cursor_x = 0;
			g_cursor_y++;
		}
		if (g_cursor_y >= TERM_ROWS) {
			term_scroll();
			g_cursor_y = TERM_ROWS - 1;
		}
		g_grid[g_cursor_y][g_cursor_x++] = ch;
	}
	if (g_cursor_y >= TERM_ROWS) {
		term_scroll();
		g_cursor_y = TERM_ROWS - 1;
	}
}

static int terminal_window_w(void)
{
	return TERM_COLS * TERM_GLYPH_W + 2 * TERM_PAD;
}

static int terminal_window_h(void)
{
	return TERM_ROWS * TERM_GLYPH_H + 2 * TERM_PAD + TITLE_H;
}

static int terminal_visible(void)
{
	return !g_terminal_closed && !g_terminal_minimized;
}

static void clamp_terminal_position(void)
{
	int max_x = (int)g_info.width - terminal_window_w();
	int max_y = (int)g_info.height - TASKBAR_H - terminal_window_h();

	if (max_x < 0)
		max_x = 0;
	if (max_y < 0)
		max_y = 0;
	if (g_term_x < 0)
		g_term_x = 0;
	if (g_term_y < 0)
		g_term_y = 0;
	if (g_term_x > max_x)
		g_term_x = max_x;
	if (g_term_y > max_y)
		g_term_y = max_y;
}

static void draw_title_button(uint32_t *target,
                              int x,
                              int y,
                              uint32_t bg,
                              int close_button)
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

static void render_terminal(uint32_t *target)
{
	int win_w = terminal_window_w();
	int win_h = terminal_window_h();
	int control_y;

	if (!terminal_visible())
		return;

	fill_rect(target, g_term_x, g_term_y, win_w, TITLE_H, COLOR_TITLEBAR);
	draw_text(target,
	          g_term_x + 8,
	          g_term_y + 2,
	          "Terminal",
	          COLOR_TITLEBAR_FG,
	          COLOR_TITLEBAR);
	control_y = drunix_window_control_y(g_term_y, TITLE_H);
	draw_title_button(target,
	                  drunix_window_minimize_button_x(g_term_x, win_w),
	                  control_y,
	                  COLOR_TITLEBAR_BUTTON,
	                  0);
	draw_title_button(target,
	                  drunix_window_close_button_x(g_term_x, win_w),
	                  control_y,
	                  COLOR_TITLEBAR_CLOSE,
	                  1);

	fill_rect(target,
	          g_term_x,
	          g_term_y + TITLE_H,
	          win_w,
	          win_h - TITLE_H,
	          COLOR_TERM_BG);

	int gx = g_term_x + TERM_PAD;
	int gy = g_term_y + TITLE_H + TERM_PAD;
	for (int r = 0; r < TERM_ROWS; r++) {
		for (int c = 0; c < TERM_COLS; c++) {
			draw_glyph(target,
			           gx + c * TERM_GLYPH_W,
			           gy + r * TERM_GLYPH_H,
			           g_grid[r][c],
			           COLOR_TERM_FG,
			           COLOR_TERM_BG);
		}
	}

	int cx = gx + g_cursor_x * TERM_GLYPH_W;
	int cy = gy + g_cursor_y * TERM_GLYPH_H;
	fill_rect(
	    target, cx, cy + TERM_GLYPH_H - 2, TERM_GLYPH_W, 2, COLOR_TERM_FG);
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
	return (int)g_info.height - TASKBAR_H + 8;
}

static int taskbar_terminal_x(void)
{
	return TASKBAR_PAD + TASKBAR_ICON_SIZE + TASKBAR_ICON_GAP * 2;
}

static int point_in_terminal_taskbar_button(int x, int y)
{
	return drunix_point_in_rect(x,
	                            y,
	                            taskbar_terminal_x(),
	                            taskbar_icon_y(),
	                            TASKBAR_ICON_SIZE,
	                            TASKBAR_ICON_SIZE);
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
	int x = TASKBAR_PAD;
	char clock[8];
	int clock_x;

	if (y < 0)
		return;
	fill_rect(target, 0, y, (int)g_info.width, TASKBAR_H, COLOR_TASKBAR);
	fill_rect(target, 0, y, (int)g_info.width, 1, COLOR_TASKBAR_EDGE);

	draw_taskbar_button(target, x, icon_y, 0, draw_taskbar_icon_logo);
	x += TASKBAR_ICON_SIZE + TASKBAR_ICON_GAP * 2;
	draw_taskbar_button(
	    target, x, icon_y, terminal_visible(), draw_taskbar_icon_terminal);
	x += TASKBAR_ICON_SIZE + TASKBAR_ICON_GAP;
	draw_taskbar_button(target, x, icon_y, 0, draw_taskbar_icon_file);
	x += TASKBAR_ICON_SIZE + TASKBAR_ICON_GAP;
	draw_taskbar_button(target, x, icon_y, 0, draw_taskbar_icon_gear);
	x += TASKBAR_ICON_SIZE + TASKBAR_ICON_GAP;
	draw_taskbar_button(target, x, icon_y, 0, draw_taskbar_icon_browser);

	format_clock(clock, (int)sizeof(clock));
	clock_x = (int)g_info.width - TASKBAR_PAD - 5 * TERM_GLYPH_W;
	draw_text(
	    target, clock_x, y + 16, clock, COLOR_TASKBAR_CLOCK, COLOR_TASKBAR);
}

static void draw_pointer_sprite(void);

static void compose_scene(void)
{
	if (!g_scene)
		return;
	if (g_wallpaper)
		memcpy(g_scene, g_wallpaper, g_fb_bytes);
	else
		render_wallpaper(g_scene);
	render_terminal(g_scene);
	render_taskbar(g_scene);
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
	/*
	 * Pointer overlay is drawn only into the live framebuffer.  The scene
	 * buffer keeps the desktop/window pixels underneath it.
	 */
	for (int j = 0; j < POINTER_H; j++) {
		for (int i = 0; i < POINTER_W; i++) {
			int cursor_pixel = drunix_cursor_pixel_at(i, j);

			if (cursor_pixel == DRUNIX_CURSOR_PIXEL_FG)
				put_pixel(g_fb,
				          g_pointer_x + i,
				          g_pointer_y + j,
				          COLOR_CURSOR);
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
	int n;

	fd = sys_open("/dev/fb0info");
	if (fd < 0)
		return -1;
	while (read_total < (int)sizeof(g_info)) {
		n = sys_read(
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

static int spawn_shell(int slave_fd, char *const *argv, char *const *envp)
{
	int pid = sys_fork();

	if (pid < 0) {
		sys_write("desktop: fork failed\n");
		sys_exit(1);
	}
	if (pid == 0) {
		sys_dup2(slave_fd, 0);
		sys_dup2(slave_fd, 1);
		sys_dup2(slave_fd, 2);
		if (slave_fd > 2)
			sys_close(slave_fd);
		sys_execve("bin/shell", (char **)argv, (char **)envp);
		sys_write("desktop: exec failed\n");
		sys_exit(1);
	}
	return pid;
}

/*
 * Forward declarations for the three event-emitter children.  Each one
 * runs in its own forked process and pumps fixed 4-byte records into
 * the shared event pipe that the parent reads.
 */
static void emit_event(int pipe_w, char tag, uint8_t a, uint8_t b, uint8_t c)
{
	uint8_t rec[4];

	rec[0] = (uint8_t)tag;
	rec[1] = a;
	rec[2] = b;
	rec[3] = c;
	sys_fwrite(pipe_w, (const char *)rec, (int)sizeof(rec));
}

static int fd_has_input(int fd)
{
	sys_pollfd_t pfd;

	pfd.fd = fd;
	pfd.events = SYS_POLLIN;
	pfd.revents = 0;
	return sys_poll(&pfd, 1, 0) > 0 && (pfd.revents & SYS_POLLIN);
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

static void drain_terminal_bytes(int fd)
{
	uint8_t buf[512];
	int rendered = 0;

	for (;;) {
		int n = sys_read(fd, (char *)buf, (int)sizeof(buf));
		if (n <= 0)
			break;
		for (int i = 0; i < n; i++)
			term_putchar((char)buf[i]);
		rendered = 1;
		if (!fd_has_input(fd))
			break;
	}

	if (rendered && terminal_visible()) {
		render_terminal(g_scene);
		copy_rect_from_scene(
		    g_term_x, g_term_y, terminal_window_w(), terminal_window_h());
		render_pointer();
	}
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
		int i;

		if (n <= 0)
			continue;
		records = n / (int)sizeof(struct evt);
		for (i = 0; i < records; i++) {
			struct evt *e = &batch[i];
			char cooked[KBDMAP_OUT_MIN];
			int produced;

			if (e->type != 0x01 /* EV_KEY */)
				continue;
			produced = kbdmap_translate(
			    &kstate, e->code, e->value, cooked, (int)sizeof(cooked));
			for (int j = 0; j < produced; j++)
				emit_event(pipe_w, EVT_KEY, (uint8_t)cooked[j], 0, 0);
		}
	}
}

static void run_term_helper(int pipe_w, int master_fd)
{
	uint8_t buf[256];

	for (;;) {
		int n = sys_read(master_fd, (char *)buf, (int)sizeof(buf));
		if (n <= 0)
			continue;
		sys_fwrite(pipe_w, (const char *)buf, n);
	}
}

static void run_mouse_helper(int pipe_w)
{
	int mfd = sys_open("/dev/mouse");
	/*
	 * Linux input_event ABI on i386: { sec, usec, type, code, value }
	 * — 16 bytes total.  /dev/mouse rejects reads smaller than one
	 * record, so we always pull a multiple of sizeof(rec) and never
	 * desync from report boundaries.  Each report ends in
	 * EV_SYN/SYN_REPORT.  We accumulate REL_X / REL_Y / button
	 * changes, clamp the deltas into signed bytes for the parent's
	 * fixed 4-byte event format, and flush on SYN_REPORT.
	 */
	struct evt {
		uint32_t sec;
		uint32_t usec;
		uint16_t type;
		uint16_t code;
		int32_t value;
	};
	struct evt batch[16];

	if (mfd < 0)
		sys_exit(1);

	int32_t pending_dx = 0;
	int32_t pending_dy = 0;
	uint8_t pending_buttons = 0;

	for (;;) {
		int n = sys_read(mfd, (char *)batch, (int)sizeof(batch));
		int records;
		int i;

		if (n <= 0)
			continue;
		records = n / (int)sizeof(struct evt);
		for (i = 0; i < records; i++) {
			struct evt *e = &batch[i];

			if (e->type == 0x02 /* EV_REL */) {
				if (e->code == 0x00 /* REL_X */)
					pending_dx += e->value;
				else if (e->code == 0x01 /* REL_Y */)
					pending_dy += e->value;
			} else if (e->type == 0x01 /* EV_KEY */) {
				uint8_t mask = 0;

				if (e->code == 0x110 /* BTN_LEFT */)
					mask = 0x01u;
				else if (e->code == 0x111 /* BTN_RIGHT */)
					mask = 0x02u;
				else if (e->code == 0x112 /* BTN_MIDDLE */)
					mask = 0x04u;
				if (e->value)
					pending_buttons |= mask;
				else
					pending_buttons &= (uint8_t)~mask;
			} else if (e->type == 0x00 /* EV_SYN */) {
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
		fn(arg0, arg1);
		sys_exit(0);
	}
	return pid;
}

/*
 * Wrappers so spawn_helper_child can take a single uniform signature.
 */
static void wrap_kbd(int pipe_w, int unused)
{
	(void)unused;
	run_kbd_helper(pipe_w);
}

static void wrap_term(int pipe_w, int master_fd)
{
	run_term_helper(pipe_w, master_fd);
}

static void wrap_mouse(int pipe_w, int unused)
{
	(void)unused;
	run_mouse_helper(pipe_w);
}

static void close_terminal_window(void)
{
	if (g_terminal_closed)
		return;
	g_terminal_closed = 1;
	g_terminal_minimized = 0;
	g_dragging_terminal = 0;
	if (g_shell_pid > 0)
		sys_kill(g_shell_pid, SIGTERM);
	if (g_term_helper_pid > 0)
		sys_kill(g_term_helper_pid, SIGTERM);
	if (g_ptmx >= 0) {
		sys_close(g_ptmx);
		g_ptmx = -1;
	}
	if (g_term_pipe_r >= 0) {
		sys_close(g_term_pipe_r);
		g_term_pipe_r = -1;
	}
}

static void handle_mouse_event(uint8_t buttons, int8_t sdx, int8_t sdy)
{
	uint8_t old_buttons = g_mouse_buttons;
	int old_x = g_pointer_x;
	int old_y = g_pointer_y;
	int full_repaint = 0;

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

	if (g_dragging_terminal && (old_buttons & 0x01u)) {
		int dx = g_pointer_x - old_x;
		int dy = g_pointer_y - old_y;

		if (dx != 0 || dy != 0) {
			g_term_x += dx;
			g_term_y += dy;
			clamp_terminal_position();
			full_repaint = 1;
		}
	}
	if (g_dragging_terminal && !(buttons & 0x01u))
		g_dragging_terminal = 0;

	if ((buttons & 0x01u) && !(old_buttons & 0x01u)) {
		if (!g_terminal_closed &&
		    point_in_terminal_taskbar_button(g_pointer_x, g_pointer_y)) {
			g_terminal_minimized = terminal_visible();
			full_repaint = 1;
		} else if (terminal_visible()) {
			int hit = drunix_window_hit_test(g_term_x,
			                                 g_term_y,
			                                 terminal_window_w(),
			                                 terminal_window_h(),
			                                 TITLE_H,
			                                 g_pointer_x,
			                                 g_pointer_y);

			if (hit == DRUNIX_WINDOW_HIT_CLOSE) {
				close_terminal_window();
				full_repaint = 1;
			} else if (hit == DRUNIX_WINDOW_HIT_MINIMIZE) {
				g_terminal_minimized = 1;
				g_dragging_terminal = 0;
				full_repaint = 1;
			} else if (hit == DRUNIX_WINDOW_HIT_TITLE) {
				g_dragging_terminal = 1;
			}
		}
	}

	g_mouse_buttons = buttons;
	if (full_repaint)
		present_scene();
	else
		render_pointer();
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (read_fb_info() != 0) {
		sys_write("desktop: cannot read /dev/fb0info\n");
		return 1;
	}

	int fbfd = sys_open_flags("/dev/fb0", SYS_O_RDWR, 0);
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
	if (sys_display_claim() != 0) {
		sys_write("desktop: cannot claim display\n");
		return 1;
	}

	g_scene = (uint32_t *)malloc(g_fb_bytes);
	if (!g_scene) {
		sys_write("desktop: scene allocation failed\n");
		return 1;
	}

	g_wallpaper = (uint32_t *)malloc(g_fb_bytes);
	if (g_wallpaper)
		render_wallpaper(g_wallpaper);

	g_ptmx = sys_open_flags("/dev/ptmx", SYS_O_RDWR, 0);
	if (g_ptmx < 0) {
		sys_write("desktop: cannot open /dev/ptmx\n");
		return 1;
	}
	int slave = sys_open_flags("/dev/pts0", SYS_O_RDWR, 0);
	if (slave < 0) {
		sys_write("desktop: cannot open /dev/pts0\n");
		return 1;
	}

	char *shell_argv[] = {"shell", 0};
	char *shell_envp[] = {"PATH=/usr/bin:/bin", 0};
	g_shell_pid = spawn_shell(slave, shell_argv, shell_envp);
	sys_close(slave);

	int evt_fds[2];
	if (sys_pipe(evt_fds) != 0) {
		sys_write("desktop: pipe failed\n");
		return 1;
	}
	int term_fds[2];
	if (sys_pipe(term_fds) != 0) {
		sys_write("desktop: pipe failed\n");
		return 1;
	}

	int evt_r = evt_fds[0];
	int evt_w = evt_fds[1];
	int term_w = term_fds[1];
	g_term_pipe_r = term_fds[0];

	(void)spawn_helper_child(wrap_kbd, evt_w, 0);
	g_term_helper_pid = spawn_helper_child(wrap_term, term_w, g_ptmx);
	(void)spawn_helper_child(wrap_mouse, evt_w, 0);
	sys_close(evt_w);
	sys_close(term_w);

	g_pointer_x = (int)g_info.width / 2;
	g_pointer_y = (int)g_info.height / 2;
	g_pointer_old_x = g_pointer_x;
	g_pointer_old_y = g_pointer_y;

	term_clear();
	present_scene();

	for (;;) {
		sys_pollfd_t pfds[2];
		int nfds = 0;
		int term_poll_idx = -1;
		int evt_poll_idx;

		if (g_term_pipe_r >= 0) {
			term_poll_idx = nfds;
			pfds[nfds].fd = g_term_pipe_r;
			pfds[nfds].events = SYS_POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}
		evt_poll_idx = nfds;
		pfds[nfds].fd = evt_r;
		pfds[nfds].events = SYS_POLLIN;
		pfds[nfds].revents = 0;
		nfds++;

		if (sys_poll(pfds, (unsigned int)nfds, -1) <= 0) {
			sys_yield();
			continue;
		}

		if (term_poll_idx >= 0 && (pfds[term_poll_idx].revents & SYS_POLLIN))
			drain_terminal_bytes(g_term_pipe_r);

		while ((pfds[evt_poll_idx].revents & SYS_POLLIN) ||
		       fd_has_input(evt_r)) {
			uint8_t rec[4];

			pfds[evt_poll_idx].revents = 0;
			if (read_event_record(evt_r, rec) != 0)
				break;

			switch (rec[0]) {
			case EVT_KEY:
				if (terminal_visible() && g_ptmx >= 0)
					sys_fwrite(g_ptmx, (const char *)&rec[1], 1);
				break;
			case EVT_MOUSE: {
				/*
				 * Coalesced report from the mouse helper:
				 *   rec[1] = current button mask (low 3 bits),
				 *   rec[2] = signed dx (REL_X, positive = right),
				 *   rec[3] = signed dy (REL_Y, positive = down).
				 *
				 * REL_Y is already in screen orientation (the kernel
				 * driver flips the PS/2 wire convention before
				 * publishing), so we just add it directly.
				 */
				int8_t sdx = (int8_t)rec[2];
				int8_t sdy = (int8_t)rec[3];

				handle_mouse_event(rec[1], sdx, sdy);
				break;
			}
			default:
				break;
			}
		}
	}

	return 0;
}

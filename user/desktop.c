/*
 * desktop.c — DrunixOS user-space compositor.
 *
 * Maps /dev/fb0, runs a shell on a freshly-allocated pty pair, and
 * renders the slave-side output into a windowed terminal on top of
 * a procedural / JPEG wallpaper.  Three forked helpers stream events
 * into a shared pipe so the parent can multiplex without poll(2):
 *
 *   - keyboard helper:  /dev/kbd → 'K' tagged events
 *   - terminal helper:  pty master → 'T' tagged events
 *   - mouse helper:     /dev/mouse → 'M' tagged 3-byte packets
 *
 * The parent reads 4-byte event records from the pipe, forwards
 * keystrokes to the pty master, feeds terminal bytes through a
 * minimal ANSI consumer, and tracks the pointer position so the
 * cursor sprite follows the mouse.
 *
 * Framebuffer geometry comes from /dev/fb0info at startup; nothing
 * about the screen is hard-coded.
 */

#include "desktop_font.h"
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
#define COLOR_CURSOR 0x00ffffffu
#define COLOR_CURSOR_SHADOW 0x00202020u

#define TERM_COLS 80
#define TERM_ROWS 24
#define TERM_GLYPH_W 8
#define TERM_GLYPH_H 16
#define TERM_PAD 8
#define TITLE_H 20
#define TERM_X 64
#define TERM_Y 64

#define POINTER_W 8
#define POINTER_H 12

#define EVT_TERM 'T'
#define EVT_KEY 'K'
#define EVT_MOUSE 'M'

static uint32_t *g_fb;
static uint32_t *g_wallpaper;
static fbinfo_t g_info;
static uint32_t g_pitch_pixels;

static char g_grid[TERM_ROWS][TERM_COLS];
static int g_cursor_x;
static int g_cursor_y;
static int g_esc_state;

static int g_pointer_x;
static int g_pointer_y;
static int g_pointer_old_x;
static int g_pointer_old_y;

static uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << g_info.red_pos) |
	       ((uint32_t)g << g_info.green_pos) |
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

static void
copy_rect_from_wallpaper(int x, int y, int w, int h)
{
	if (!g_wallpaper)
		return;
	if (w <= 0 || h <= 0)
		return;
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= (int)g_info.height)
			continue;
		uint32_t *src = g_wallpaper + (uint32_t)yy * g_pitch_pixels;
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
			uint32_t color = (bits & (0x80u >> col)) ? fg : bg;
			put_pixel(target, x + col, y + row, color);
		}
	}
}

static void
draw_text(uint32_t *target, int x, int y, const char *s, uint32_t fg, uint32_t bg)
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
		const uint8_t *srow =
		    rgb + sy * (uint32_t)src_w * (color ? 3u : 1u);
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

static void render_terminal(void)
{
	int win_w = TERM_COLS * TERM_GLYPH_W + 2 * TERM_PAD;
	int win_h = TERM_ROWS * TERM_GLYPH_H + 2 * TERM_PAD + TITLE_H;

	fill_rect(g_fb, TERM_X, TERM_Y, win_w, TITLE_H, COLOR_TITLEBAR);
	draw_text(
	    g_fb, TERM_X + 8, TERM_Y + 2, "Shell", COLOR_TITLEBAR_FG, COLOR_TITLEBAR);

	fill_rect(g_fb,
	          TERM_X,
	          TERM_Y + TITLE_H,
	          win_w,
	          win_h - TITLE_H,
	          COLOR_TERM_BG);

	int gx = TERM_X + TERM_PAD;
	int gy = TERM_Y + TITLE_H + TERM_PAD;
	for (int r = 0; r < TERM_ROWS; r++) {
		for (int c = 0; c < TERM_COLS; c++) {
			draw_glyph(g_fb,
			           gx + c * TERM_GLYPH_W,
			           gy + r * TERM_GLYPH_H,
			           g_grid[r][c],
			           COLOR_TERM_FG,
			           COLOR_TERM_BG);
		}
	}

	int cx = gx + g_cursor_x * TERM_GLYPH_W;
	int cy = gy + g_cursor_y * TERM_GLYPH_H;
	fill_rect(g_fb, cx, cy + TERM_GLYPH_H - 2, TERM_GLYPH_W, 2, COLOR_TERM_FG);
}

static void render_pointer(void)
{
	/*
	 * A trivial pointed cursor: a vertical white wedge with a darker
	 * shadow column.  Cleared by repainting the rectangle from the
	 * cached wallpaper buffer before the new draw, so motion does not
	 * smear over the wallpaper.
	 */
	if (g_pointer_old_x != g_pointer_x || g_pointer_old_y != g_pointer_y)
		copy_rect_from_wallpaper(
		    g_pointer_old_x, g_pointer_old_y, POINTER_W, POINTER_H);

	for (int j = 0; j < POINTER_H; j++) {
		int row_w = POINTER_W - j / 2;
		if (row_w <= 0)
			break;
		fill_rect(g_fb, g_pointer_x, g_pointer_y + j, row_w, 1, COLOR_CURSOR);
		fill_rect(g_fb,
		          g_pointer_x + row_w,
		          g_pointer_y + j,
		          1,
		          1,
		          COLOR_CURSOR_SHADOW);
	}
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
		n = sys_read(fd,
		             (char *)&g_info + read_total,
		             (int)sizeof(g_info) - read_total);
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

static void
spawn_shell(int slave_fd, char *const *argv, char *const *envp)
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
				emit_event(
				    pipe_w, EVT_KEY, (uint8_t)cooked[j], 0, 0);
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
		for (int i = 0; i < n; i++)
			emit_event(pipe_w, EVT_TERM, buf[i], 0, 0);
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
	uint32_t fb_bytes = g_info.pitch * g_info.height;
	g_fb = (uint32_t *)mmap(
	    0, fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (g_fb == MAP_FAILED) {
		sys_write("desktop: framebuffer mmap failed\n");
		return 1;
	}

	g_wallpaper = (uint32_t *)malloc(fb_bytes);
	if (g_wallpaper)
		render_wallpaper(g_wallpaper);

	int ptmx = sys_open_flags("/dev/ptmx", SYS_O_RDWR, 0);
	if (ptmx < 0) {
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
	spawn_shell(slave, shell_argv, shell_envp);
	sys_close(slave);

	int evt_fds[2];
	if (sys_pipe(evt_fds) != 0) {
		sys_write("desktop: pipe failed\n");
		return 1;
	}

	int evt_r = evt_fds[0];
	int evt_w = evt_fds[1];

	(void)spawn_helper_child(wrap_kbd, evt_w, 0);
	(void)spawn_helper_child(wrap_term, evt_w, ptmx);
	(void)spawn_helper_child(wrap_mouse, evt_w, 0);

	g_pointer_x = (int)g_info.width / 2;
	g_pointer_y = (int)g_info.height / 2;
	g_pointer_old_x = g_pointer_x;
	g_pointer_old_y = g_pointer_y;

	term_clear();
	if (g_wallpaper)
		memcpy(g_fb, g_wallpaper, fb_bytes);
	else
		render_wallpaper(g_fb);
	render_terminal();
	render_pointer();

	for (;;) {
		uint8_t rec[4];
		int got = 0;

		while (got < 4) {
			int n = sys_read(evt_r, (char *)(rec + got), 4 - got);
			if (n <= 0) {
				got = 0;
				break;
			}
			got += n;
		}
		if (got != 4)
			continue;

		switch (rec[0]) {
		case EVT_TERM:
			term_putchar((char)rec[1]);
			render_terminal();
			render_pointer();
			break;
		case EVT_KEY:
			sys_fwrite(ptmx, (const char *)&rec[1], 1);
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
			render_pointer();
			break;
		}
		default:
			break;
		}
	}

	return 0;
}

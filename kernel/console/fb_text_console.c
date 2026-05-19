#include "fb_text_console.h"
#include "kstring.h"
#include <limits.h>

static int fb_text_console_cell_count(const fb_text_console_t *console,
                                      uint32_t *cell_count)
{
	uint64_t total;

	if (!console || !cell_count || console->cols == 0u || console->rows == 0u)
		return 0;

	total = (uint64_t)console->cols * (uint64_t)console->rows;
	if (total == 0u || total > UINT32_MAX)
		return 0;

	*cell_count = (uint32_t)total;
	return 1;
}

static void fb_text_console_present_rect(fb_text_console_t *console,
                                         uint32_t col,
                                         uint32_t row,
                                         uint32_t cols,
                                         uint32_t rows)
{
	uint32_t px;
	uint32_t py;
	uint32_t pw;
	uint32_t ph;

	if (!console || !console->fb)
		return;

	gui_display_present_rect_to_framebuffer(&console->display,
	                                        console->fb,
	                                        (int)col,
	                                        (int)row,
	                                        (int)cols,
	                                        (int)rows);
	px = col * GUI_FONT_W;
	py = row * GUI_FONT_H;
	pw = cols * GUI_FONT_W;
	ph = rows * GUI_FONT_H;
	framebuffer_present_rect(console->fb, (int)px, (int)py, (int)pw, (int)ph);

	/* Platform dirty-rect hook (M7 polish): lets BCM2712-class
	 * non-coherent display engines do targeted dc cvac on the modified
	 * pixel range instead of paying for a full framebuffer sweep per
	 * character. Clamp the rect to framebuffer bounds before the
	 * call. */
	if (console->dirty_pixels) {
		if (px < console->fb->width && py < console->fb->height) {
			if (px + pw > console->fb->width)
				pw = console->fb->width - px;
			if (py + ph > console->fb->height)
				ph = console->fb->height - py;
			console->dirty_pixels(px, py, pw, ph);
		}
	}
}

static void fb_text_console_present_all(fb_text_console_t *console)
{
	if (!console)
		return;

	fb_text_console_present_rect(console, 0u, 0u, console->cols, console->rows);
}

static gui_cell_t *fb_text_console_cell(fb_text_console_t *console,
                                        uint32_t col,
                                        uint32_t row)
{
	if (!console || !console->cells || col >= console->cols || row >= console->rows)
		return 0;

	return &console->cells[row * console->cols + col];
}

static void fb_text_console_clear_row(fb_text_console_t *console, uint32_t row)
{
	gui_cell_t blank;
	uint32_t base;

	if (!console || !console->cells || row >= console->rows)
		return;

	blank.ch = ' ';
	blank.attr = console->attr;
	base = row * console->cols;
	for (uint32_t col = 0u; col < console->cols; col++)
		console->cells[base + col] = blank;
}

static void fb_text_console_scroll(fb_text_console_t *console)
{
	uint32_t row_bytes;

	if (!console || !console->cells || console->rows == 0u)
		return;

	if (console->rows > 1u) {
		row_bytes = console->cols * (uint32_t)sizeof(gui_cell_t);
		k_memmove(console->cells,
		          console->cells + console->cols,
		          row_bytes * (console->rows - 1u));
	}
	fb_text_console_clear_row(console, console->rows - 1u);
	console->cursor_row = console->rows - 1u;

	if (console->scroll_pixels && console->scroll_pixels(console) == 0)
		return;

	/*
	 * Match modern Linux fbcon's unaccelerated path: redraw from the
	 * software text buffer instead of doing soft copyarea from scanout.
	 * fbcon intentionally avoids soft copyarea unless framebuffer reads
	 * are known fast; Pi firmware scanout memory is not a good read source.
	 */
	fb_text_console_present_all(console);
}

static void fb_text_console_advance_row(fb_text_console_t *console)
{
	if (!console)
		return;

	console->wrap_pending = 0;
	console->cursor_row++;
	if (console->cursor_row >= console->rows)
		fb_text_console_scroll(console);
}

static void fb_text_console_apply_pending_wrap(fb_text_console_t *console)
{
	if (!console || !console->wrap_pending)
		return;

	console->wrap_pending = 0;
	console->cursor_col = 0u;
	fb_text_console_advance_row(console);
}

static void fb_text_console_put_cell(fb_text_console_t *console, char ch)
{
	gui_cell_t *cell;
	uint32_t col;
	uint32_t row;

	if (!console || console->cols == 0u || console->rows == 0u)
		return;

	fb_text_console_apply_pending_wrap(console);

	col = console->cursor_col;
	row = console->cursor_row;
	cell = fb_text_console_cell(console, col, row);
	if (!cell)
		return;

	cell->ch = ch;
	cell->attr = console->attr;
	fb_text_console_present_rect(console, col, row, 1u, 1u);

	if (console->cursor_col + 1u >= console->cols)
		console->wrap_pending = 1;
	else
		console->cursor_col++;
}

static void fb_text_console_backspace(fb_text_console_t *console)
{
	gui_cell_t *cell;

	if (!console)
		return;

	if (console->wrap_pending) {
		console->wrap_pending = 0;
	} else if (console->cursor_col > 0u) {
		console->cursor_col--;
	} else if (console->cursor_row > 0u) {
		console->cursor_row--;
		console->cursor_col = console->cols - 1u;
	} else {
		return;
	}

	cell = fb_text_console_cell(console, console->cursor_col, console->cursor_row);
	if (!cell)
		return;

	cell->ch = ' ';
	cell->attr = console->attr;
	fb_text_console_present_rect(console,
	                             console->cursor_col,
	                             console->cursor_row,
	                             1u,
	                             1u);
}

int fb_text_console_init(fb_text_console_t *console,
                         framebuffer_info_t *fb,
                         gui_cell_t *cells,
                         uint32_t cell_capacity)
{
	uint64_t total_cells;

	if (!console || !fb || !cells)
		return -1;
	if (fb->cell_cols == 0u || fb->cell_rows == 0u)
		return -1;
	if (fb->cell_cols > (uint32_t)INT_MAX ||
	    fb->cell_rows > (uint32_t)INT_MAX)
		return -1;
	if (fb->cell_cols > (uint32_t)(INT_MAX / GUI_FONT_W) ||
	    fb->cell_rows > (uint32_t)(INT_MAX / GUI_FONT_H))
		return -1;

	total_cells = (uint64_t)fb->cell_cols * (uint64_t)fb->cell_rows;
	if (total_cells == 0u || total_cells > (uint64_t)INT_MAX ||
	    cell_capacity < (uint32_t)total_cells)
		return -1;

	k_memset(console, 0, sizeof(*console));
	console->cells = cells;
	console->fb = fb;
	console->cols = fb->cell_cols;
	console->rows = fb->cell_rows;
	console->attr = 0x0f;
	console->ready = 1;

	gui_display_init(&console->display,
	                 console->cells,
	                 (int)console->cols,
	                 (int)console->rows,
	                 console->attr);
	fb_text_console_clear(console);
	return 0;
}

void fb_text_console_set_dirty_pixels(fb_text_console_t *console,
                                      fb_text_console_dirty_pixels_fn fn)
{
	if (!console)
		return;
	console->dirty_pixels = fn;
}

void fb_text_console_set_scroll_pixels(fb_text_console_t *console,
                                       fb_text_console_scroll_pixels_fn fn)
{
	if (!console)
		return;
	console->scroll_pixels = fn;
}

int fb_text_console_ready(const fb_text_console_t *console)
{
	uint32_t cell_count;

	if (!console || !console->ready || !console->fb || !console->cells)
		return 0;

	return fb_text_console_cell_count(console, &cell_count);
}

void fb_text_console_write(fb_text_console_t *console,
                           const char *buf,
                           uint32_t len)
{
	if (!fb_text_console_ready(console) || !buf)
		return;

	for (uint32_t i = 0u; i < len; i++) {
		unsigned char ch = (unsigned char)buf[i];

		if (console->ansi_state == 1) {
			console->ansi_state = (ch == '[') ? 2 : 0;
			continue;
		}
		if (console->ansi_state == 2) {
			if (ch >= 0x40u && ch <= 0x7eu)
				console->ansi_state = 0;
			continue;
		}
		if (ch == 0x1bu) {
			console->ansi_state = 1;
			continue;
		}
		if (ch == '\r') {
			console->cursor_col = 0u;
			console->wrap_pending = 0;
			continue;
		}
		if (ch == '\n') {
			console->cursor_col = 0u;
			fb_text_console_advance_row(console);
			continue;
		}
		if (ch == '\b' || ch == 0x7fu) {
			fb_text_console_backspace(console);
			continue;
		}
		if (ch < 32u || ch > 126u)
			ch = ' ';

		fb_text_console_put_cell(console, (char)ch);
	}
}

void fb_text_console_clear(fb_text_console_t *console)
{
	uint32_t cell_count;

	if (!fb_text_console_ready(console))
		return;
	if (!fb_text_console_cell_count(console, &cell_count))
		return;

	for (uint32_t i = 0u; i < cell_count; i++) {
		console->cells[i].ch = ' ';
		console->cells[i].attr = console->attr;
	}
	console->cursor_col = 0u;
	console->cursor_row = 0u;
	console->wrap_pending = 0;
	console->ansi_state = 0;
	gui_display_set_cursor(&console->display, 0, 0, 1);
	fb_text_console_present_all(console);
}

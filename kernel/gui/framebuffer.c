#include "framebuffer.h"
#include "font8x16.h"
#include "kstring.h"
#include <limits.h>

static int rgb_mask_overlaps(uint32_t a_pos,
                             uint32_t a_size,
                             uint32_t b_pos,
                             uint32_t b_size)
{
	uint32_t a_end = a_pos + a_size;
	uint32_t b_end = b_pos + b_size;

	return a_pos < b_end && b_pos < a_end;
}

static uint64_t div_u64_by_255(uint64_t value)
{
	uint64_t quotient = 0;
	uint64_t remainder = 0;

	for (int bit = 63; bit >= 0; bit--) {
		remainder = (remainder << 1) | ((value >> bit) & 1ull);
		if (remainder >= 255ull) {
			remainder -= 255ull;
			quotient |= 1ull << bit;
		}
	}
	return quotient;
}

static uint64_t div_u64_by_u64(uint64_t numerator, uint64_t denominator)
{
	uint64_t quotient = 0;
	uint64_t remainder = 0;

	if (denominator == 0)
		return 0;
	for (int bit = 63; bit >= 0; bit--) {
		remainder = (remainder << 1) | ((numerator >> bit) & 1ull);
		if (remainder >= denominator) {
			remainder -= denominator;
			quotient |= 1ull << bit;
		}
	}
	return quotient;
}

static uint32_t scale_color(uint8_t value, uint8_t mask_size)
{
	uint64_t max;
	uint64_t scaled;

	if (mask_size == 0)
		return 0;
	if (mask_size >= 32)
		max = UINT32_MAX;
	else
		max = (1ull << mask_size) - 1ull;
	scaled = (uint64_t)value * max + 127ull;
	return (uint32_t)div_u64_by_255(scaled);
}

int framebuffer_info_from_rgb(uintptr_t address,
                              uint32_t pitch,
                              uint32_t width,
                              uint32_t height,
                              uint32_t bpp,
                              uint8_t red_pos,
                              uint8_t red_size,
                              uint8_t green_pos,
                              uint8_t green_size,
                              uint8_t blue_pos,
                              uint8_t blue_size,
                              framebuffer_info_t *out)
{
	uint64_t visible_row_bytes;
	uint64_t last_row_offset;
	uint64_t framebuffer_bytes;

	if (!out)
		return -1;
	if (address == 0)
		return -3;
	if (bpp != 32)
		return -5;
	if (width == 0 || height == 0)
		return -6;
	if (width > UINT32_MAX / 4u)
		return -7;
	if (pitch < width * 4u)
		return -7;
	visible_row_bytes = (uint64_t)width * 4u;
	last_row_offset = (uint64_t)(height - 1u) * pitch;
	framebuffer_bytes = last_row_offset + visible_row_bytes;
	if (framebuffer_bytes == 0 ||
	    framebuffer_bytes - 1u > (uint64_t)UINTPTR_MAX - address)
		return -3;
	if (width < GUI_FONT_W || height < GUI_FONT_H)
		return -8;
	if (red_size == 0 || green_size == 0 || blue_size == 0)
		return -9;
	if ((uint32_t)red_pos + red_size > 32u)
		return -9;
	if ((uint32_t)green_pos + green_size > 32u)
		return -9;
	if ((uint32_t)blue_pos + blue_size > 32u)
		return -9;
	if (rgb_mask_overlaps(red_pos, red_size, green_pos, green_size))
		return -9;
	if (rgb_mask_overlaps(red_pos, red_size, blue_pos, blue_size))
		return -9;
	if (rgb_mask_overlaps(green_pos, green_size, blue_pos, blue_size))
		return -9;

	k_memset(out, 0, sizeof(*out));
	out->address = address;
	out->pitch = pitch;
	out->width = width;
	out->height = height;
	out->bpp = bpp;
	out->red_pos = red_pos;
	out->red_size = red_size;
	out->green_pos = green_pos;
	out->green_size = green_size;
	out->blue_pos = blue_pos;
	out->blue_size = blue_size;
	out->cell_cols = width / GUI_FONT_W;
	out->cell_rows = height / GUI_FONT_H;
	return 0;
}

uint32_t framebuffer_pack_rgb(const framebuffer_info_t *fb,
                              uint8_t r,
                              uint8_t g,
                              uint8_t b)
{
	if (!fb)
		return 0;
	return (scale_color(r, fb->red_size) << fb->red_pos) |
	       (scale_color(g, fb->green_size) << fb->green_pos) |
	       (scale_color(b, fb->blue_size) << fb->blue_pos);
}

int framebuffer_has_back_buffer(const framebuffer_info_t *fb)
{
	return fb && fb->back_address != 0 && fb->back_pitch != 0;
}

uintptr_t framebuffer_draw_address(const framebuffer_info_t *fb)
{
	if (!fb)
		return 0;
	if (framebuffer_has_back_buffer(fb))
		return fb->back_address;
	return fb->address;
}

uint32_t framebuffer_draw_pitch(const framebuffer_info_t *fb)
{
	if (!fb)
		return 0;
	if (framebuffer_has_back_buffer(fb))
		return fb->back_pitch;
	return fb->pitch;
}

int framebuffer_attach_back_buffer(framebuffer_info_t *fb,
                                   void *buffer,
                                   uint32_t pitch,
                                   uint32_t capacity_bytes)
{
	uint64_t min_row_bytes;
	uint64_t min_total_bytes;

	if (!fb)
		return -1;
	if (!buffer || pitch == 0) {
		fb->back_address = 0;
		fb->back_pitch = 0;
		return 0;
	}
	if (fb->width == 0 || fb->height == 0)
		return -1;

	/* Refuse to attach a buffer that can't hold the whole framebuffer. */
	min_row_bytes = (uint64_t)fb->width * 4u;
	if (pitch < min_row_bytes)
		return -2;
	min_total_bytes = (uint64_t)(fb->height - 1u) * pitch + min_row_bytes;
	if (capacity_bytes < min_total_bytes)
		return -3;

	fb->back_address = (uintptr_t)buffer;
	fb->back_pitch = pitch;
	return 0;
}

void framebuffer_fill_rect(
    const framebuffer_info_t *fb, int x, int y, int w, int h, uint32_t color)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;
	uintptr_t base;
	uint32_t row_pitch;
	uint32_t *row_ptr;

	if (!fb || w <= 0 || h <= 0)
		return;
	base = framebuffer_draw_address(fb);
	row_pitch = framebuffer_draw_pitch(fb);
	if (base == 0 || row_pitch == 0)
		return;
	left = x;
	top = y;
	right = left + (int64_t)w;
	bottom = top + (int64_t)h;
	if (right <= 0 || bottom <= 0)
		return;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > (int64_t)fb->width)
		right = (int64_t)fb->width;
	if (bottom > (int64_t)fb->height)
		bottom = (int64_t)fb->height;
	if (left >= right || top >= bottom)
		return;

	for (int64_t row = top; row < bottom; row++) {
		row_ptr = (uint32_t *)(base + (uintptr_t)row * row_pitch);
		row_ptr += (uintptr_t)left;
		k_memset32(row_ptr, color, (uint32_t)(right - left));
	}
}

static void framebuffer_composite_cursor_row(const framebuffer_info_t *fb,
                                             uintptr_t dst_row_base,
                                             int64_t row,
                                             int64_t row_left,
                                             int64_t row_right)
{
	const framebuffer_cursor_t *cursor;
	int64_t cursor_row;
	int64_t cursor_left;
	int64_t cursor_right;
	uint32_t *pixels;

	if (!fb)
		return;
	cursor = &fb->cursor;
	if (!cursor->visible)
		return;

	cursor_row = row - cursor->y;
	if (cursor_row < 0 || cursor_row >= FRAMEBUFFER_CURSOR_H)
		return;

	cursor_left = cursor->x;
	cursor_right = cursor_left + FRAMEBUFFER_CURSOR_W;
	if (cursor_left < row_left)
		cursor_left = row_left;
	if (cursor_right > row_right)
		cursor_right = row_right;
	if (cursor_left >= cursor_right)
		return;

	pixels = (uint32_t *)dst_row_base;
	for (int64_t px = cursor_left; px < cursor_right; px++) {
		int cursor_col = (int)(px - cursor->x);
		int cursor_pixel;

		cursor_pixel = drunix_cursor_pixel_at(cursor_col, (int)cursor_row);
		if (cursor_pixel == DRUNIX_CURSOR_PIXEL_FG)
			pixels[px - row_left] = cursor->fg;
		else if (cursor_pixel == DRUNIX_CURSOR_PIXEL_SHADOW)
			pixels[px - row_left] = cursor->shadow;
	}
}

#ifdef KTEST_ENABLED
static uint32_t g_framebuffer_present_count;
#endif

void framebuffer_present_rect(
    const framebuffer_info_t *fb, int x, int y, int w, int h)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;
	uint32_t row_bytes;

	if (!framebuffer_has_back_buffer(fb) || w <= 0 || h <= 0)
		return;
	if (fb->address == 0 || fb->pitch == 0)
		return;

	left = x;
	top = y;
	right = left + (int64_t)w;
	bottom = top + (int64_t)h;
	if (right <= 0 || bottom <= 0)
		return;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > (int64_t)fb->width)
		right = (int64_t)fb->width;
	if (bottom > (int64_t)fb->height)
		bottom = (int64_t)fb->height;
	if (left >= right || top >= bottom)
		return;

#ifdef KTEST_ENABLED
	g_framebuffer_present_count++;
#endif

	row_bytes = (uint32_t)(right - left) * 4u;
	for (int64_t row = top; row < bottom; row++) {
		uintptr_t src = fb->back_address + (uintptr_t)row * fb->back_pitch +
		                (uintptr_t)left * 4u;
		uintptr_t dst =
		    fb->address + (uintptr_t)row * fb->pitch + (uintptr_t)left * 4u;

		k_memcpy((void *)dst, (const void *)src, row_bytes);

		/*
         * Composite the cursor overlay on top of the just-copied row. The
         * overlay never touches the back buffer, so moving the cursor is
         * just a matter of re-presenting the old and new cursor rects.
         */
		framebuffer_composite_cursor_row(fb, dst, row, left, right);
	}
}

#ifdef KTEST_ENABLED
void framebuffer_present_count_reset_for_test(void)
{
	g_framebuffer_present_count = 0;
}

uint32_t framebuffer_present_count_for_test(void)
{
	return g_framebuffer_present_count;
}
#endif

void framebuffer_set_cursor(framebuffer_info_t *fb,
                            int x,
                            int y,
                            uint32_t fg,
                            uint32_t shadow,
                            int visible)
{
	if (!fb)
		return;
	fb->cursor.x = x;
	fb->cursor.y = y;
	fb->cursor.fg = fg;
	fb->cursor.shadow = shadow;
	fb->cursor.visible = visible ? 1 : 0;
}

void framebuffer_blit_rect(const framebuffer_info_t *fb,
                           int src_x,
                           int src_y,
                           int dst_x,
                           int dst_y,
                           int w,
                           int h)
{
	int64_t sx0;
	int64_t sy0;
	int64_t dx0;
	int64_t dy0;
	int64_t width;
	int64_t height;
	uintptr_t base;
	uint32_t row_pitch;
	uint32_t row_bytes;
	int reverse;

	if (!fb || w <= 0 || h <= 0)
		return;
	base = framebuffer_draw_address(fb);
	row_pitch = framebuffer_draw_pitch(fb);
	if (base == 0 || row_pitch == 0)
		return;

	sx0 = src_x;
	sy0 = src_y;
	dx0 = dst_x;
	dy0 = dst_y;
	width = w;
	height = h;

	/*
     * Clip the blit so both the source and destination rectangles land
     * inside the framebuffer. We trim equal amounts off matching edges so
     * the source and destination pixels stay aligned.
     */
	if (sx0 < 0) {
		width += sx0;
		dx0 -= sx0;
		sx0 = 0;
	}
	if (sy0 < 0) {
		height += sy0;
		dy0 -= sy0;
		sy0 = 0;
	}
	if (dx0 < 0) {
		width += dx0;
		sx0 -= dx0;
		dx0 = 0;
	}
	if (dy0 < 0) {
		height += dy0;
		sy0 -= dy0;
		dy0 = 0;
	}
	if (sx0 + width > (int64_t)fb->width)
		width = (int64_t)fb->width - sx0;
	if (sy0 + height > (int64_t)fb->height)
		height = (int64_t)fb->height - sy0;
	if (dx0 + width > (int64_t)fb->width)
		width = (int64_t)fb->width - dx0;
	if (dy0 + height > (int64_t)fb->height)
		height = (int64_t)fb->height - dy0;
	if (width <= 0 || height <= 0)
		return;

	row_bytes = (uint32_t)width * 4u;
	/*
     * Overlap handling: when the destination is below the source, we must
     * copy rows from bottom to top so earlier rows are read before being
     * overwritten. k_memmove handles horizontal overlap within a row.
     */
	reverse = dy0 > sy0;
	for (int64_t i = 0; i < height; i++) {
		int64_t row = reverse ? (height - 1 - i) : i;
		uintptr_t src =
		    base + (uintptr_t)(sy0 + row) * row_pitch + (uintptr_t)sx0 * 4u;
		uintptr_t dst =
		    base + (uintptr_t)(dy0 + row) * row_pitch + (uintptr_t)dx0 * 4u;

		k_memmove((void *)dst, (const void *)src, row_bytes);
	}
}

static gui_pixel_rect_t framebuffer_clip_pixel_rect(
    const framebuffer_info_t *fb, int x, int y, int w, int h)
{
	gui_pixel_rect_t out = {0, 0, 0, 0};
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	if (!fb || w <= 0 || h <= 0)
		return out;
	left = x;
	top = y;
	right = left + (int64_t)w;
	bottom = top + (int64_t)h;
	if (left >= (int64_t)fb->width || top >= (int64_t)fb->height)
		return out;
	if (right <= 0 || bottom <= 0)
		return out;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > (int64_t)fb->width)
		right = (int64_t)fb->width;
	if (bottom > (int64_t)fb->height)
		bottom = (int64_t)fb->height;
	if (left >= right || top >= bottom)
		return out;
	if (left > INT_MAX || top > INT_MAX)
		return out;
	if (right - left > INT_MAX || bottom - top > INT_MAX)
		return out;
	out.x = (int)left;
	out.y = (int)top;
	out.w = (int)(right - left);
	out.h = (int)(bottom - top);
	return out;
}

static void framebuffer_fill_rect_clipped64(const framebuffer_info_t *fb,
                                            int64_t x,
                                            int64_t y,
                                            int64_t w,
                                            int64_t h,
                                            uint32_t color)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	if (!fb || framebuffer_draw_address(fb) == 0 || w <= 0 || h <= 0)
		return;
	left = x;
	top = y;
	right = left + w;
	bottom = top + h;
	if (right <= 0 || bottom <= 0)
		return;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > (int64_t)fb->width)
		right = (int64_t)fb->width;
	if (bottom > (int64_t)fb->height)
		bottom = (int64_t)fb->height;
	if (left >= right || top >= bottom)
		return;
	if (left > INT_MAX || top > INT_MAX)
		return;
	if (right - left > INT_MAX || bottom - top > INT_MAX)
		return;
	framebuffer_fill_rect(fb,
	                      (int)left,
	                      (int)top,
	                      (int)(right - left),
	                      (int)(bottom - top),
	                      color);
}

void framebuffer_draw_rect_outline(
    const framebuffer_info_t *fb, int x, int y, int w, int h, uint32_t color)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	if (!fb || w <= 0 || h <= 0)
		return;
	left = x;
	top = y;
	right = left + (int64_t)w;
	bottom = top + (int64_t)h;
	if (right <= left || bottom <= top)
		return;
	framebuffer_fill_rect_clipped64(fb, left, top, right - left, 1, color);
	framebuffer_fill_rect_clipped64(
	    fb, left, bottom - 1, right - left, 1, color);
	framebuffer_fill_rect_clipped64(fb, left, top, 1, bottom - top, color);
	framebuffer_fill_rect_clipped64(fb, right - 1, top, 1, bottom - top, color);
}

static void framebuffer_draw_glyph_clipped(const framebuffer_info_t *fb,
                                           const gui_pixel_rect_t *clip,
                                           int64_t x,
                                           int64_t y,
                                           unsigned char ch,
                                           uint32_t fg,
                                           uint32_t bg)
{
	const uint8_t *glyph;
	uintptr_t base;
	uint32_t row_pitch;
	int64_t fb_w;
	int64_t fb_h;
	int64_t clip_x0;
	int64_t clip_y0;
	int64_t clip_x1;
	int64_t clip_y1;
	int row_start;
	int row_end;
	int col_start;
	int col_end;

	/*
     * Compute the glyph's visible column/row range once, then run a tight
     * inner loop that writes pixels directly into the draw target. The old
     * implementation called framebuffer_fill_rect_clipped64() for every
     * one of the glyph's 128 pixels, which dominated CPU time for every
     * text-heavy repaint.
     */
	if (!fb || !clip)
		return;
	base = framebuffer_draw_address(fb);
	row_pitch = framebuffer_draw_pitch(fb);
	if (base == 0 || row_pitch == 0)
		return;

	fb_w = (int64_t)fb->width;
	fb_h = (int64_t)fb->height;

	clip_x0 = clip->x > 0 ? clip->x : 0;
	clip_y0 = clip->y > 0 ? clip->y : 0;
	clip_x1 = (int64_t)clip->x + clip->w;
	if (clip_x1 > fb_w)
		clip_x1 = fb_w;
	clip_y1 = (int64_t)clip->y + clip->h;
	if (clip_y1 > fb_h)
		clip_y1 = fb_h;
	if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
		return;

	if (y >= clip_y1 || y + 16 <= clip_y0)
		return;
	if (x >= clip_x1 || x + 8 <= clip_x0)
		return;

	row_start = (int)(clip_y0 - y);
	if (row_start < 0)
		row_start = 0;
	row_end = (int)(clip_y1 - y);
	if (row_end > 16)
		row_end = 16;

	col_start = (int)(clip_x0 - x);
	if (col_start < 0)
		col_start = 0;
	col_end = (int)(clip_x1 - x);
	if (col_end > 8)
		col_end = 8;
	if (col_start >= col_end || row_start >= row_end)
		return;

	glyph = font8x16_glyph(ch);
	for (int row = row_start; row < row_end; row++) {
		uint8_t bits = glyph[row];
		int64_t py = y + row;
		uint32_t *pixels;

		pixels = (uint32_t *)(base + (uintptr_t)py * row_pitch);
		pixels += (uintptr_t)(x + col_start);
		for (int col = col_start; col < col_end; col++)
			pixels[col - col_start] = (bits & (1u << col)) ? fg : bg;
	}
}

void framebuffer_draw_text_clipped(const framebuffer_info_t *fb,
                                   const gui_pixel_rect_t *clip,
                                   int x,
                                   int y,
                                   const char *text,
                                   uint32_t fg,
                                   uint32_t bg)
{
	gui_pixel_rect_t bounded;
	int64_t cursor_x;
	int64_t clip_right;
	int col = 0;

	if (!fb || !clip || !text)
		return;
	bounded =
	    framebuffer_clip_pixel_rect(fb, clip->x, clip->y, clip->w, clip->h);
	if (bounded.w <= 0 || bounded.h <= 0)
		return;
	cursor_x = x;
	clip_right = (int64_t)bounded.x + (int64_t)bounded.w;
	while (text[col]) {
		if (cursor_x > INT_MAX - 7)
			break;
		if (cursor_x + 7 < bounded.x) {
			cursor_x += GUI_FONT_W;
			col++;
			continue;
		}
		if (cursor_x >= clip_right)
			break;
		framebuffer_draw_glyph_clipped(
		    fb, &bounded, cursor_x, y, (unsigned char)text[col], fg, bg);
		cursor_x += GUI_FONT_W;
		col++;
	}
}

void framebuffer_draw_scrollbar(const framebuffer_info_t *fb,
                                int x,
                                int y,
                                int w,
                                int h,
                                int total_rows,
                                int visible_rows,
                                int view_top,
                                uint32_t track,
                                uint32_t thumb)
{
	uint64_t thumb_h;
	int64_t thumb_y;
	uint64_t max_top;
	uint64_t travel;
	uint64_t total;
	uint64_t visible;
	uint64_t view_top_u;

	if (!fb || w <= 0 || h <= 0)
		return;
	framebuffer_fill_rect_clipped64(fb, x, y, w, h, track);
	if (total_rows <= 0 || visible_rows <= 0 || total_rows <= visible_rows)
		return;
	total = total_rows;
	visible = visible_rows;
	if (view_top < 0)
		view_top = 0;
	max_top = total - visible;
	view_top_u = (uint64_t)view_top;
	if (view_top_u > max_top)
		view_top_u = max_top;
	thumb_h = div_u64_by_u64((uint64_t)h * visible, total);
	if (thumb_h < 8)
		thumb_h = h < 8 ? h : 8;
	if (thumb_h > (uint64_t)h)
		thumb_h = (uint64_t)h;
	travel = (uint64_t)h - thumb_h;
	thumb_y = y;
	if (max_top > 0)
		thumb_y += (int64_t)div_u64_by_u64(travel * view_top_u, max_top);
	framebuffer_fill_rect_clipped64(fb, x, thumb_y, w, thumb_h, thumb);
}

void framebuffer_draw_glyph(const framebuffer_info_t *fb,
                            int x,
                            int y,
                            unsigned char ch,
                            uint32_t fg,
                            uint32_t bg)
{
	gui_pixel_rect_t clip;

	if (!fb)
		return;
	if (x > INT_MAX - 7 || y > INT_MAX - 15)
		return;

	/*
     * Delegate to the clipped variant so the fast-path inner loop stays in
     * one place. The clip is set to the whole framebuffer so nothing is
     * actually excluded, but the visible-range precomputation still lets
     * us run one uint32_t store per pixel with no function-call overhead.
     */
	clip.x = 0;
	clip.y = 0;
	clip.w = (int)fb->width;
	clip.h = (int)fb->height;
	framebuffer_draw_glyph_clipped(fb, &clip, x, y, ch, fg, bg);
}

void framebuffer_draw_cursor(
    const framebuffer_info_t *fb, int x, int y, uint32_t fg, uint32_t shadow)
{
	if (x > INT_MAX - (DRUNIX_CURSOR_W - 1) ||
	    y > INT_MAX - (DRUNIX_CURSOR_H - 1))
		return;

	for (int row = 0; row < DRUNIX_CURSOR_H; row++) {
		for (int col = 0; col < DRUNIX_CURSOR_W; col++) {
			int cursor_pixel = drunix_cursor_pixel_at(col, row);
			if (cursor_pixel == DRUNIX_CURSOR_PIXEL_FG)
				framebuffer_fill_rect(fb, x + col, y + row, 1, 1, fg);
			else if (cursor_pixel == DRUNIX_CURSOR_PIXEL_SHADOW)
				framebuffer_fill_rect(
				    fb, x + col, y + row, 1, 1, shadow);
		}
	}
}

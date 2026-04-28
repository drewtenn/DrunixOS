/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "drwin_gfx.h"
#include "desktop_font.h"

static const uint8_t fallback_blank[DRWIN_GLYPH_H];
static const uint8_t fallback_box[DRWIN_GLYPH_H] = {
    0x7e,
    0x42,
    0x5a,
    0x5a,
    0x42,
    0x42,
    0x42,
    0x42,
    0x42,
    0x42,
    0x42,
    0x42,
    0x5a,
    0x5a,
    0x42,
    0x7e,
};

__attribute__((weak)) const uint8_t *desktop_font_glyph(unsigned char ch)
{
	return ch == ' ' ? fallback_blank : fallback_box;
}

static int valid_surface(const drwin_surface_t *surface)
{
	int64_t min_pitch;

	if (!surface || !surface->pixels || surface->width <= 0 ||
	    surface->height <= 0 || surface->pitch <= 0 ||
	    surface->bpp != DRWIN_BPP)
		return 0;
	min_pitch = (int64_t)surface->width * 4;
	if ((int64_t)surface->pitch < min_pitch)
		return 0;
	return 1;
}

void drwin_fill_rect(drwin_surface_t *surface,
                     int x,
                     int y,
                     int w,
                     int h,
                     uint32_t color)
{
	int64_t x0 = x;
	int64_t y0 = y;
	int64_t x1 = (int64_t)x + w;
	int64_t y1 = (int64_t)y + h;

	if (!valid_surface(surface) || w <= 0 || h <= 0)
		return;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > surface->width)
		x1 = surface->width;
	if (y1 > surface->height)
		y1 = surface->height;
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int yy = (int)y0; yy < (int)y1; yy++) {
		uint8_t *row = (uint8_t *)surface->pixels + yy * surface->pitch;
		uint32_t *px = (uint32_t *)row;

		for (int xx = (int)x0; xx < (int)x1; xx++)
			px[xx] = color;
	}
}

static void draw_glyph(drwin_surface_t *surface,
                       int64_t x,
                       int64_t y,
                       const uint8_t *glyph,
                       uint32_t fg,
                       uint32_t bg)
{
	if (!glyph)
		return;
	for (int gy = 0; gy < DRWIN_GLYPH_H; gy++) {
		int64_t yy64 = y + gy;
		int yy;
		uint8_t bits;

		if (yy64 < 0 || yy64 >= surface->height)
			continue;
		yy = (int)yy64;
		bits = glyph[gy];
		for (int gx = 0; gx < DRWIN_GLYPH_W; gx++) {
			int64_t xx64 = x + gx;
			int xx;
			uint8_t mask = (uint8_t)(1u << gx);
			uint8_t *row;
			uint32_t *px;

			if (xx64 < 0 || xx64 >= surface->width)
				continue;
			xx = (int)xx64;
			row = (uint8_t *)surface->pixels + yy * surface->pitch;
			px = (uint32_t *)row;
			px[xx] = (bits & mask) ? fg : bg;
		}
	}
}

void drwin_draw_text(drwin_surface_t *surface,
                     int x,
                     int y,
                     const char *text,
                     uint32_t fg,
                     uint32_t bg)
{
	int64_t pen_x = x;
	int64_t pen_y = y;

	if (!valid_surface(surface) || !text)
		return;
	for (int i = 0; text[i]; i++) {
		unsigned char ch = (unsigned char)text[i];

		if (ch == '\n') {
			pen_x = x;
			pen_y += DRWIN_GLYPH_H;
			continue;
		}
		if (ch == '\r') {
			pen_x = x;
			continue;
		}
		if (pen_x < surface->width && pen_x + DRWIN_GLYPH_W > 0 &&
		    pen_y < surface->height && pen_y + DRWIN_GLYPH_H > 0)
			draw_glyph(surface, pen_x, pen_y, desktop_font_glyph(ch), fg, bg);
		pen_x += DRWIN_GLYPH_W;
	}
}

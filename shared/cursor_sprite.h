/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRUNIX_CURSOR_SPRITE_H
#define DRUNIX_CURSOR_SPRITE_H

#define DRUNIX_CURSOR_W 13
#define DRUNIX_CURSOR_H 20

#define DRUNIX_CURSOR_PIXEL_TRANSPARENT 0
#define DRUNIX_CURSOR_PIXEL_FG 1
#define DRUNIX_CURSOR_PIXEL_SHADOW 2

static const char drunix_cursor_sprite[DRUNIX_CURSOR_H][DRUNIX_CURSOR_W + 1] = {
    "W............",
    "WX...........",
    "WWX..........",
    "WWWX.........",
    "WWWWX........",
    "WWWWWX.......",
    "WWWWWWX......",
    "WWWWWWWX.....",
    "WWWWWWWWX....",
    "WWWWWWWWWX...",
    "WWWWWWWWWWX..",
    "WWWWWXXXXX...",
    "WWWX.........",
    "WWXX.........",
    "WX.XX........",
    "X...XX.......",
    "....XX.......",
    ".....XX......",
    ".....XX......",
    ".............",
};

static inline int drunix_cursor_pixel_at(int x, int y)
{
	if (x < 0 || y < 0 || x >= DRUNIX_CURSOR_W || y >= DRUNIX_CURSOR_H)
		return DRUNIX_CURSOR_PIXEL_TRANSPARENT;

	if (drunix_cursor_sprite[y][x] == 'W')
		return DRUNIX_CURSOR_PIXEL_FG;
	if (drunix_cursor_sprite[y][x] == 'X')
		return DRUNIX_CURSOR_PIXEL_SHADOW;
	return DRUNIX_CURSOR_PIXEL_TRANSPARENT;
}

#endif /* DRUNIX_CURSOR_SPRITE_H */

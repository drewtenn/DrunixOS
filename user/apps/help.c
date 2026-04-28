/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "drwin.h"
#include "drwin_gfx.h"

#define HELP_W 320
#define HELP_H 220
#define HELP_BG 0x00141414u
#define HELP_FG 0x00d0d0d0u
#define HELP_ACCENT 0x0060d6ffu

static int open_window(const char *title,
                       int w,
                       int h,
                       drwin_window_t *win,
                       drwin_surface_t *surface)
{
	if (drwin_connect() != 0)
		return -1;
	if (drwin_create_window(title, 160, 128, w, h, win) != 0)
		return -1;
	if (drwin_map_surface(*win, surface) != 0) {
		drwin_destroy_window(*win);
		return -1;
	}
	return 0;
}

static void draw_help(drwin_surface_t *surface)
{
	int y = 12;

	drwin_fill_rect(surface, 0, 0, surface->width, surface->height, HELP_BG);
	drwin_draw_text(surface, 12, y, "Help", HELP_ACCENT, HELP_BG);
	y += DRWIN_GLYPH_H * 2;
	drwin_draw_text(surface, 12, y, "Terminal runs shell", HELP_FG, HELP_BG);
	y += DRWIN_GLYPH_H;
	drwin_draw_text(surface, 12, y, "Files lists root", HELP_FG, HELP_BG);
	y += DRWIN_GLYPH_H;
	drwin_draw_text(surface, 12, y, "Processes lists /proc", HELP_FG, HELP_BG);
	y += DRWIN_GLYPH_H;
	drwin_draw_text(surface, 12, y, "Close windows from the titlebar", HELP_FG, HELP_BG);
}

int main(void)
{
	drwin_window_t win;
	drwin_surface_t surface;
	drwin_event_t event;

	if (open_window("Help", HELP_W, HELP_H, &win, &surface) != 0)
		return 1;
	draw_help(&surface);
	drwin_present(win, (drwin_rect_t){0, 0, surface.width, surface.height});

	while (drwin_poll_event(&event, -1) > 0) {
		if (event.type == DRWIN_EVENT_CLOSE ||
		    event.type == DRWIN_EVENT_DISCONNECT)
			break;
	}
	drwin_destroy_window(win);
	return 0;
}

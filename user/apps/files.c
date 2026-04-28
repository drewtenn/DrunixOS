/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "drwin.h"
#include "drwin_gfx.h"
#include "syscall.h"

#define FILES_W 320
#define FILES_H 220
#define FILES_BG 0x00141414u
#define FILES_FG 0x00d0d0d0u
#define FILES_MUTED 0x0088a0a8u

static int open_window(const char *title,
                       int w,
                       int h,
                       drwin_window_t *win,
                       drwin_surface_t *surface)
{
	if (drwin_connect() != 0)
		return -1;
	if (drwin_create_window(title, 96, 96, w, h, win) != 0)
		return -1;
	if (drwin_map_surface(*win, surface) != 0) {
		drwin_destroy_window(*win);
		return -1;
	}
	return 0;
}

static void draw_directory(drwin_surface_t *surface,
                           const char *path,
                           const char *heading)
{
	char dents[512];
	int n = sys_getdents(path, dents, (int)sizeof(dents));
	int y = 12;

	drwin_fill_rect(surface, 0, 0, surface->width, surface->height, FILES_BG);
	drwin_draw_text(surface, 12, y, heading, FILES_FG, FILES_BG);
	y += DRWIN_GLYPH_H * 2;
	if (n <= 0) {
		drwin_draw_text(surface, 12, y, "(empty)", FILES_MUTED, FILES_BG);
		return;
	}
	for (int i = 0; i < n && y + DRWIN_GLYPH_H < surface->height;) {
		const char *entry = dents + i;
		int len = 0;

		while (i + len < n && entry[len])
			len++;
		if (i + len >= n)
			break;
		drwin_draw_text(surface, 12, y, entry, FILES_FG, FILES_BG);
		y += DRWIN_GLYPH_H;
		i += len + 1;
	}
}

int main(void)
{
	drwin_window_t win;
	drwin_surface_t surface;
	drwin_event_t event;

	if (open_window("Files", FILES_W, FILES_H, &win, &surface) != 0)
		return 1;
	draw_directory(&surface, "/", "Files: /");
	drwin_present(win, (drwin_rect_t){0, 0, surface.width, surface.height});

	while (drwin_poll_event(&event, -1) > 0) {
		if (event.type == DRWIN_EVENT_CLOSE ||
		    event.type == DRWIN_EVENT_DISCONNECT)
			break;
	}
	drwin_destroy_window(win);
	return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRWIN_H
#define DRWIN_H

#include "wm_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int width;
	int height;
	int pitch;
	int bpp;
	void *pixels;
} drwin_surface_t;

int drwin_connect(void);
int drwin_create_window(const char *title,
                        int x,
                        int y,
                        int w,
                        int h,
                        drwin_window_t *out);
int drwin_map_surface(drwin_window_t window, drwin_surface_t *out);
int drwin_present(drwin_window_t window, drwin_rect_t dirty);
int drwin_poll_event(drwin_event_t *event, int timeout_ms);
int drwin_set_title(drwin_window_t window, const char *title);
int drwin_show_window(drwin_window_t window, int visible);
int drwin_destroy_window(drwin_window_t window);
int drwin_fd_for_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* DRWIN_H */

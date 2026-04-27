/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRUNIX_DESKTOP_WINDOW_H
#define DRUNIX_DESKTOP_WINDOW_H

#define DRUNIX_WINDOW_HIT_NONE 0
#define DRUNIX_WINDOW_HIT_BODY 1
#define DRUNIX_WINDOW_HIT_TITLE 2
#define DRUNIX_WINDOW_HIT_MINIMIZE 3
#define DRUNIX_WINDOW_HIT_CLOSE 4

#define DRUNIX_WINDOW_CONTROL_SIZE 12
#define DRUNIX_WINDOW_CONTROL_PAD 6
#define DRUNIX_WINDOW_CONTROL_GAP 6

static inline int drunix_window_close_button_x(int window_x, int window_w)
{
	return window_x + window_w - DRUNIX_WINDOW_CONTROL_PAD -
	       DRUNIX_WINDOW_CONTROL_SIZE;
}

static inline int drunix_window_minimize_button_x(int window_x, int window_w)
{
	return drunix_window_close_button_x(window_x, window_w) -
	       DRUNIX_WINDOW_CONTROL_GAP - DRUNIX_WINDOW_CONTROL_SIZE;
}

static inline int drunix_window_control_y(int window_y, int title_h)
{
	return window_y + (title_h - DRUNIX_WINDOW_CONTROL_SIZE) / 2;
}

static inline int drunix_point_in_rect(int px,
                                       int py,
                                       int x,
                                       int y,
                                       int w,
                                       int h)
{
	return px >= x && py >= y && px < x + w && py < y + h;
}

static inline int drunix_window_hit_test(int window_x,
                                         int window_y,
                                         int window_w,
                                         int window_h,
                                         int title_h,
                                         int px,
                                         int py)
{
	int control_y;
	int close_x;
	int minimize_x;

	if (!drunix_point_in_rect(px, py, window_x, window_y, window_w, window_h))
		return DRUNIX_WINDOW_HIT_NONE;

	if (!drunix_point_in_rect(px, py, window_x, window_y, window_w, title_h))
		return DRUNIX_WINDOW_HIT_BODY;

	control_y = drunix_window_control_y(window_y, title_h);
	close_x = drunix_window_close_button_x(window_x, window_w);
	minimize_x = drunix_window_minimize_button_x(window_x, window_w);

	if (drunix_point_in_rect(px,
	                         py,
	                         close_x,
	                         control_y,
	                         DRUNIX_WINDOW_CONTROL_SIZE,
	                         DRUNIX_WINDOW_CONTROL_SIZE))
		return DRUNIX_WINDOW_HIT_CLOSE;
	if (drunix_point_in_rect(px,
	                         py,
	                         minimize_x,
	                         control_y,
	                         DRUNIX_WINDOW_CONTROL_SIZE,
	                         DRUNIX_WINDOW_CONTROL_SIZE))
		return DRUNIX_WINDOW_HIT_MINIMIZE;
	return DRUNIX_WINDOW_HIT_TITLE;
}

#endif /* DRUNIX_DESKTOP_WINDOW_H */

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRUNIX_DESKTOP_WALLPAPER_H
#define DRUNIX_DESKTOP_WALLPAPER_H

typedef struct {
	int x;
	int y;
} drunix_wallpaper_sample_t;

static inline int drunix_wallpaper_ceil_div(int value, int divisor)
{
	return (value + divisor - 1) / divisor;
}

static inline int drunix_wallpaper_clamp(int value, int max_exclusive)
{
	if (value < 0)
		return 0;
	if (value >= max_exclusive)
		return max_exclusive - 1;
	return value;
}

static inline drunix_wallpaper_sample_t
drunix_wallpaper_cover_sample(int dst_x,
                              int dst_y,
                              int dst_w,
                              int dst_h,
                              int src_w,
                              int src_h)
{
	drunix_wallpaper_sample_t sample = {0, 0};

	if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
		return sample;

	if ((unsigned int)dst_w * (unsigned int)src_h >=
	    (unsigned int)dst_h * (unsigned int)src_w) {
		int scaled_h = drunix_wallpaper_ceil_div(src_h * dst_w, src_w);
		int crop_y = (scaled_h - dst_h) / 2;

		sample.x = (dst_x * src_w) / dst_w;
		sample.y = ((dst_y + crop_y) * src_w) / dst_w;
	} else {
		int scaled_w = drunix_wallpaper_ceil_div(src_w * dst_h, src_h);
		int crop_x = (scaled_w - dst_w) / 2;

		sample.x = ((dst_x + crop_x) * src_h) / dst_h;
		sample.y = (dst_y * src_h) / dst_h;
	}

	sample.x = drunix_wallpaper_clamp(sample.x, src_w);
	sample.y = drunix_wallpaper_clamp(sample.y, src_h);
	return sample;
}

#endif /* DRUNIX_DESKTOP_WALLPAPER_H */

/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRWIN_GFX_H
#define DRWIN_GFX_H

#include "drwin.h"
#include "stdint.h"

#define DRWIN_GLYPH_W 8
#define DRWIN_GLYPH_H 16

#ifdef __cplusplus
extern "C" {
#endif

void drwin_fill_rect(drwin_surface_t *surface,
                     int x,
                     int y,
                     int w,
                     int h,
                     uint32_t color);
void drwin_draw_text(drwin_surface_t *surface,
                     int x,
                     int y,
                     const char *text,
                     uint32_t fg,
                     uint32_t bg);

#ifdef __cplusplus
}
#endif

#endif /* DRWIN_GFX_H */

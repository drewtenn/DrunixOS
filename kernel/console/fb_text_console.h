#ifndef CONSOLE_FB_TEXT_CONSOLE_H
#define CONSOLE_FB_TEXT_CONSOLE_H

#include "display.h"
#include "framebuffer.h"
#include <stdint.h>

/*
 * Optional dirty-rect callback fired after each present_rect. Lets the
 * platform driver scope cache maintenance (e.g. dc cvac on BCM2712,
 * where the HVS is not L3-coherent) to the exact pixel area that
 * fb_text_console just modified, instead of paying for a full
 * framebuffer sweep per character. The rect coordinates are in
 * pixels relative to the framebuffer's top-left; the rect is already
 * clipped to the framebuffer bounds. Caller must tolerate a null
 * pointer here (older platforms that don't set a hook).
 */
typedef void (*fb_text_console_dirty_pixels_fn)(uint32_t x,
                                                uint32_t y,
                                                uint32_t w,
                                                uint32_t h);

typedef struct fb_text_console {
	gui_display_t display;
	gui_cell_t *cells;
	framebuffer_info_t *fb;
	uint32_t cols;
	uint32_t rows;
	uint32_t cursor_col;
	uint32_t cursor_row;
	uint8_t attr;
	int wrap_pending;
	int ansi_state;
	int ready;
	fb_text_console_dirty_pixels_fn dirty_pixels;
} fb_text_console_t;

/*
 * Register the platform's dirty-pixel hook. Replaces any previous
 * registration; pass NULL to disable. Safe to call before or after
 * fb_text_console_init.
 */
void fb_text_console_set_dirty_pixels(fb_text_console_t *console,
                                      fb_text_console_dirty_pixels_fn fn);

int fb_text_console_init(fb_text_console_t *console,
                         framebuffer_info_t *fb,
                         gui_cell_t *cells,
                         uint32_t cell_capacity);
int fb_text_console_ready(const fb_text_console_t *console);
void fb_text_console_write(fb_text_console_t *console,
                           const char *buf,
                           uint32_t len);
void fb_text_console_clear(fb_text_console_t *console);

#endif

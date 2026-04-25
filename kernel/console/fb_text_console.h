#ifndef CONSOLE_FB_TEXT_CONSOLE_H
#define CONSOLE_FB_TEXT_CONSOLE_H

#include "display.h"
#include "framebuffer.h"
#include <stdint.h>

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
} fb_text_console_t;

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

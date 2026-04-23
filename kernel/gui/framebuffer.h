#ifndef GUI_FRAMEBUFFER_H
#define GUI_FRAMEBUFFER_H

typedef struct multiboot_info multiboot_info_t;

#include "pixel.h"
#include "pmm.h"
#include <stdint.h>

#define GUI_FONT_W 8u
#define GUI_FONT_H 16u

/*
 * Virtual "hardware cursor" overlay.
 *
 * Real VGA/VBE framebuffer modes don't expose a true hardware cursor plane,
 * but we can fake one: the cursor sprite is never drawn into the back
 * buffer, and framebuffer_present_rect() composites it onto the front
 * framebuffer during the back→front copy. Moving the cursor then only
 * requires re-presenting the old and new cursor rects from the back buffer
 * — no back-buffer repaints, no rasterisation work per motion event.
 */
#define FRAMEBUFFER_CURSOR_W 8
#define FRAMEBUFFER_CURSOR_H 12

typedef struct framebuffer_cursor {
	int x;
	int y;
	uint32_t fg;
	uint32_t shadow;
	int visible;
} framebuffer_cursor_t;

typedef struct framebuffer_info {
	uintptr_t address;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t bpp;
	uint8_t red_pos;
	uint8_t red_size;
	uint8_t green_pos;
	uint8_t green_size;
	uint8_t blue_pos;
	uint8_t blue_size;
	uint32_t cell_cols;
	uint32_t cell_rows;
	/*
     * Optional off-screen back buffer. When back_address is non-zero, every
     * framebuffer_* primitive writes to back_address with back_pitch bytes
     * per row. The caller is then responsible for copying dirty rects to the
     * visible framebuffer via framebuffer_present_rect(). This eliminates
     * tearing and flicker caused by clear-then-draw sequences being visible
     * to the display scan-out.
     */
	uintptr_t back_address;
	uint32_t back_pitch;
	framebuffer_cursor_t cursor;
} framebuffer_info_t;

int framebuffer_info_from_multiboot(const multiboot_info_t *mbi,
                                    framebuffer_info_t *out);
int framebuffer_attach_back_buffer(framebuffer_info_t *fb,
                                   void *buffer,
                                   uint32_t pitch,
                                   uint32_t capacity_bytes);
int framebuffer_has_back_buffer(const framebuffer_info_t *fb);
uintptr_t framebuffer_draw_address(const framebuffer_info_t *fb);
uint32_t framebuffer_draw_pitch(const framebuffer_info_t *fb);
void framebuffer_present_rect(
    const framebuffer_info_t *fb, int x, int y, int w, int h);
void framebuffer_blit_rect(const framebuffer_info_t *fb,
                           int src_x,
                           int src_y,
                           int dst_x,
                           int dst_y,
                           int w,
                           int h);
void framebuffer_set_cursor(framebuffer_info_t *fb,
                            int x,
                            int y,
                            uint32_t fg,
                            uint32_t shadow,
                            int visible);
uint32_t framebuffer_pack_rgb(const framebuffer_info_t *fb,
                              uint8_t r,
                              uint8_t g,
                              uint8_t b);
void framebuffer_fill_rect(
    const framebuffer_info_t *fb, int x, int y, int w, int h, uint32_t color);
void framebuffer_draw_glyph(const framebuffer_info_t *fb,
                            int x,
                            int y,
                            unsigned char ch,
                            uint32_t fg,
                            uint32_t bg);
void framebuffer_draw_cursor(
    const framebuffer_info_t *fb, int x, int y, uint32_t fg, uint32_t shadow);
void framebuffer_draw_rect_outline(
    const framebuffer_info_t *fb, int x, int y, int w, int h, uint32_t color);
void framebuffer_draw_text_clipped(const framebuffer_info_t *fb,
                                   const gui_pixel_rect_t *clip,
                                   int x,
                                   int y,
                                   const char *text,
                                   uint32_t fg,
                                   uint32_t bg);
void framebuffer_draw_scrollbar(const framebuffer_info_t *fb,
                                int x,
                                int y,
                                int w,
                                int h,
                                int total_rows,
                                int visible_rows,
                                int view_top,
                                uint32_t track,
                                uint32_t thumb);

#endif

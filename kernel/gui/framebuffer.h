#ifndef GUI_FRAMEBUFFER_H
#define GUI_FRAMEBUFFER_H

#include "pmm.h"
#include <stdint.h>

#define GUI_FONT_W 8u
#define GUI_FONT_H 16u

typedef struct {
    int x;
    int y;
    int w;
    int h;
} gui_pixel_rect_t;

typedef struct {
    void *pixels;
    int width;
    int height;
} gui_pixel_surface_t;

typedef struct {
    uint32_t foreground;
    uint32_t background;
} gui_pixel_theme_t;

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
} framebuffer_info_t;

int framebuffer_info_from_multiboot(const multiboot_info_t *mbi,
                                    framebuffer_info_t *out);
uint32_t framebuffer_pack_rgb(const framebuffer_info_t *fb,
                              uint8_t r, uint8_t g, uint8_t b);
void framebuffer_fill_rect(const framebuffer_info_t *fb,
                           int x, int y, int w, int h,
                           uint32_t color);
void framebuffer_draw_glyph(const framebuffer_info_t *fb,
                            int x, int y, unsigned char ch,
                            uint32_t fg, uint32_t bg);
void framebuffer_draw_cursor(const framebuffer_info_t *fb,
                             int x, int y,
                             uint32_t fg, uint32_t shadow);
void framebuffer_draw_rect_outline(const framebuffer_info_t *fb,
                                   int x, int y, int w, int h,
                                   uint32_t color);
void framebuffer_draw_text_clipped(const framebuffer_info_t *fb,
                                   const gui_pixel_rect_t *clip,
                                   int x, int y, const char *text,
                                   uint32_t fg, uint32_t bg);
void framebuffer_draw_scrollbar(const framebuffer_info_t *fb,
                                int x, int y, int w, int h,
                                int total_rows, int visible_rows,
                                int view_top, uint32_t track_color,
                                uint32_t thumb_color);

#endif

#include "framebuffer.h"
#include "font8x16.h"
#include "kstring.h"
#include <limits.h>

static int rgb_mask_overlaps(uint32_t a_pos, uint32_t a_size,
                             uint32_t b_pos, uint32_t b_size)
{
    uint32_t a_end = a_pos + a_size;
    uint32_t b_end = b_pos + b_size;

    return a_pos < b_end && b_pos < a_end;
}

static uint64_t div_u64_by_255(uint64_t value)
{
    uint64_t quotient = 0;
    uint64_t remainder = 0;

    for (int bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((value >> bit) & 1ull);
        if (remainder >= 255ull) {
            remainder -= 255ull;
            quotient |= 1ull << bit;
        }
    }
    return quotient;
}

static uint64_t div_u64_by_u64(uint64_t numerator, uint64_t denominator)
{
    uint64_t quotient = 0;
    uint64_t remainder = 0;

    if (denominator == 0)
        return 0;
    for (int bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1ull);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= 1ull << bit;
        }
    }
    return quotient;
}

static uint32_t scale_color(uint8_t value, uint8_t mask_size)
{
    uint64_t max;
    uint64_t scaled;

    if (mask_size == 0)
        return 0;
    if (mask_size >= 32)
        max = UINT32_MAX;
    else
        max = (1ull << mask_size) - 1ull;
    scaled = (uint64_t)value * max + 127ull;
    return (uint32_t)div_u64_by_255(scaled);
}

int framebuffer_info_from_multiboot(const multiboot_info_t *mbi,
                                    framebuffer_info_t *out)
{
    uint64_t visible_row_bytes;
    uint64_t last_row_offset;
    uint64_t framebuffer_bytes;

    if (!mbi || !out)
        return -1;
    if ((mbi->flags & MULTIBOOT_FLAG_FRAMEBUFFER) == 0)
        return -2;
    if (mbi->framebuffer_addr == 0)
        return -3;
    if (mbi->framebuffer_addr > UINTPTR_MAX)
        return -3;
    if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
        return -4;
    if (mbi->framebuffer_bpp != 32)
        return -5;
    if (mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0)
        return -6;
    if (mbi->framebuffer_width > UINT32_MAX / 4u)
        return -7;
    if (mbi->framebuffer_pitch < mbi->framebuffer_width * 4u)
        return -7;
    visible_row_bytes = (uint64_t)mbi->framebuffer_width * 4u;
    last_row_offset = (uint64_t)(mbi->framebuffer_height - 1u) *
                      mbi->framebuffer_pitch;
    framebuffer_bytes = last_row_offset + visible_row_bytes;
    if (framebuffer_bytes == 0 ||
        framebuffer_bytes - 1u >
            (uint64_t)UINTPTR_MAX - mbi->framebuffer_addr)
        return -3;
    if (mbi->framebuffer_width < GUI_FONT_W ||
        mbi->framebuffer_height < GUI_FONT_H)
        return -8;
    if (mbi->framebuffer_red_mask_size == 0 ||
        mbi->framebuffer_green_mask_size == 0 ||
        mbi->framebuffer_blue_mask_size == 0)
        return -9;
    if ((uint32_t)mbi->framebuffer_red_field_position +
            mbi->framebuffer_red_mask_size >
        32u)
        return -9;
    if ((uint32_t)mbi->framebuffer_green_field_position +
            mbi->framebuffer_green_mask_size >
        32u)
        return -9;
    if ((uint32_t)mbi->framebuffer_blue_field_position +
            mbi->framebuffer_blue_mask_size >
        32u)
        return -9;
    if (rgb_mask_overlaps(mbi->framebuffer_red_field_position,
                          mbi->framebuffer_red_mask_size,
                          mbi->framebuffer_green_field_position,
                          mbi->framebuffer_green_mask_size))
        return -9;
    if (rgb_mask_overlaps(mbi->framebuffer_red_field_position,
                          mbi->framebuffer_red_mask_size,
                          mbi->framebuffer_blue_field_position,
                          mbi->framebuffer_blue_mask_size))
        return -9;
    if (rgb_mask_overlaps(mbi->framebuffer_green_field_position,
                          mbi->framebuffer_green_mask_size,
                          mbi->framebuffer_blue_field_position,
                          mbi->framebuffer_blue_mask_size))
        return -9;

    k_memset(out, 0, sizeof(*out));
    out->address = (uintptr_t)mbi->framebuffer_addr;
    out->pitch = mbi->framebuffer_pitch;
    out->width = mbi->framebuffer_width;
    out->height = mbi->framebuffer_height;
    out->bpp = mbi->framebuffer_bpp;
    out->red_pos = mbi->framebuffer_red_field_position;
    out->red_size = mbi->framebuffer_red_mask_size;
    out->green_pos = mbi->framebuffer_green_field_position;
    out->green_size = mbi->framebuffer_green_mask_size;
    out->blue_pos = mbi->framebuffer_blue_field_position;
    out->blue_size = mbi->framebuffer_blue_mask_size;
    out->cell_cols = mbi->framebuffer_width / GUI_FONT_W;
    out->cell_rows = mbi->framebuffer_height / GUI_FONT_H;
    return 0;
}

uint32_t framebuffer_pack_rgb(const framebuffer_info_t *fb,
                              uint8_t r, uint8_t g, uint8_t b)
{
    if (!fb)
        return 0;
    return (scale_color(r, fb->red_size) << fb->red_pos) |
           (scale_color(g, fb->green_size) << fb->green_pos) |
           (scale_color(b, fb->blue_size) << fb->blue_pos);
}

void framebuffer_fill_rect(const framebuffer_info_t *fb,
                           int x, int y, int w, int h,
                           uint32_t color)
{
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    uint32_t *row_ptr;

    if (!fb || fb->address == 0 || w <= 0 || h <= 0)
        return;
    left = x;
    top = y;
    right = left + (int64_t)w;
    bottom = top + (int64_t)h;
    if (right <= 0 || bottom <= 0)
        return;
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > (int64_t)fb->width)
        right = (int64_t)fb->width;
    if (bottom > (int64_t)fb->height)
        bottom = (int64_t)fb->height;
    if (left >= right || top >= bottom)
        return;

    for (int64_t row = top; row < bottom; row++) {
        row_ptr = (uint32_t *)(fb->address +
                               (uintptr_t)row * fb->pitch);
        row_ptr += (uintptr_t)left;
        for (int64_t col = left; col < right; col++)
            row_ptr[col - left] = color;
    }
}

static gui_pixel_rect_t framebuffer_clip_pixel_rect(const framebuffer_info_t *fb,
                                                    int x, int y, int w, int h)
{
    gui_pixel_rect_t out = { 0, 0, 0, 0 };
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (!fb || w <= 0 || h <= 0)
        return out;
    left = x;
    top = y;
    right = left + (int64_t)w;
    bottom = top + (int64_t)h;
    if (left >= (int64_t)fb->width || top >= (int64_t)fb->height)
        return out;
    if (right <= 0 || bottom <= 0)
        return out;
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > (int64_t)fb->width)
        right = (int64_t)fb->width;
    if (bottom > (int64_t)fb->height)
        bottom = (int64_t)fb->height;
    if (left >= right || top >= bottom)
        return out;
    if (left > INT_MAX || top > INT_MAX)
        return out;
    if (right - left > INT_MAX || bottom - top > INT_MAX)
        return out;
    out.x = (int)left;
    out.y = (int)top;
    out.w = (int)(right - left);
    out.h = (int)(bottom - top);
    return out;
}

static void framebuffer_fill_rect_clipped64(const framebuffer_info_t *fb,
                                            int64_t x, int64_t y,
                                            int64_t w, int64_t h,
                                            uint32_t color)
{
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (!fb || fb->address == 0 || w <= 0 || h <= 0)
        return;
    left = x;
    top = y;
    right = left + w;
    bottom = top + h;
    if (right <= 0 || bottom <= 0)
        return;
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > (int64_t)fb->width)
        right = (int64_t)fb->width;
    if (bottom > (int64_t)fb->height)
        bottom = (int64_t)fb->height;
    if (left >= right || top >= bottom)
        return;
    if (left > INT_MAX || top > INT_MAX)
        return;
    if (right - left > INT_MAX || bottom - top > INT_MAX)
        return;
    framebuffer_fill_rect(fb, (int)left, (int)top,
                          (int)(right - left), (int)(bottom - top), color);
}

void framebuffer_draw_rect_outline(const framebuffer_info_t *fb,
                                   int x, int y, int w, int h,
                                   uint32_t color)
{
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (!fb || w <= 0 || h <= 0)
        return;
    left = x;
    top = y;
    right = left + (int64_t)w;
    bottom = top + (int64_t)h;
    if (right <= left || bottom <= top)
        return;
    framebuffer_fill_rect_clipped64(fb, left, top, right - left, 1, color);
    framebuffer_fill_rect_clipped64(fb, left, bottom - 1, right - left, 1,
                                    color);
    framebuffer_fill_rect_clipped64(fb, left, top, 1, bottom - top, color);
    framebuffer_fill_rect_clipped64(fb, right - 1, top, 1, bottom - top,
                                    color);
}

static void framebuffer_draw_glyph_clipped(const framebuffer_info_t *fb,
                                           const gui_pixel_rect_t *clip,
                                           int64_t x, int64_t y,
                                           unsigned char ch,
                                           uint32_t fg,
                                           uint32_t bg)
{
    const uint8_t *glyph;

    if (!fb || !clip || fb->address == 0)
        return;

    glyph = font8x16_glyph(ch);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        int64_t py = y + row;

        if (py < clip->y || py >= (int64_t)clip->y + clip->h)
            continue;
        for (int col = 0; col < 8; col++) {
            int64_t px = x + col;
            uint32_t color;

            if (px < clip->x || px >= (int64_t)clip->x + clip->w)
                continue;
            color = (bits & (1u << col)) ? fg : bg;
            framebuffer_fill_rect_clipped64(fb, px, py, 1, 1, color);
        }
    }
}

void framebuffer_draw_text_clipped(const framebuffer_info_t *fb,
                                   const gui_pixel_rect_t *clip,
                                   int x, int y,
                                   const char *text,
                                   uint32_t fg,
                                   uint32_t bg)
{
    gui_pixel_rect_t bounded;
    int64_t cursor_x;
    int64_t clip_right;
    int col = 0;

    if (!fb || !clip || !text)
        return;
    bounded = framebuffer_clip_pixel_rect(fb, clip->x, clip->y, clip->w,
                                           clip->h);
    if (bounded.w <= 0 || bounded.h <= 0)
        return;
    cursor_x = x;
    clip_right = (int64_t)bounded.x + (int64_t)bounded.w;
    while (text[col]) {
        if (cursor_x > INT_MAX - 7)
            break;
        if (cursor_x + 7 < bounded.x) {
            cursor_x += GUI_FONT_W;
            col++;
            continue;
        }
        if (cursor_x >= clip_right)
            break;
        framebuffer_draw_glyph_clipped(fb, &bounded, cursor_x, y,
                                       (unsigned char)text[col], fg, bg);
        cursor_x += GUI_FONT_W;
        col++;
    }
}

void framebuffer_draw_scrollbar(const framebuffer_info_t *fb,
                                int x, int y, int w, int h,
                                int total_rows,
                                int visible_rows,
                                int view_top,
                                uint32_t track,
                                uint32_t thumb)
{
    uint64_t thumb_h;
    int64_t thumb_y;
    uint64_t max_top;
    uint64_t travel;
    uint64_t total;
    uint64_t visible;
    uint64_t view_top_u;

    if (!fb || w <= 0 || h <= 0)
        return;
    framebuffer_fill_rect_clipped64(fb, x, y, w, h, track);
    if (total_rows <= 0 || visible_rows <= 0 || total_rows <= visible_rows)
        return;
    total = total_rows;
    visible = visible_rows;
    if (view_top < 0)
        view_top = 0;
    max_top = total - visible;
    view_top_u = (uint64_t)view_top;
    if (view_top_u > max_top)
        view_top_u = max_top;
    thumb_h = div_u64_by_u64((uint64_t)h * visible, total);
    if (thumb_h < 8)
        thumb_h = h < 8 ? h : 8;
    if (thumb_h > h)
        thumb_h = h;
    travel = (uint64_t)h - thumb_h;
    thumb_y = y;
    if (max_top > 0)
        thumb_y += (int64_t)div_u64_by_u64(travel * view_top_u, max_top);
    framebuffer_fill_rect_clipped64(fb, x, thumb_y, w, thumb_h, thumb);
}

void framebuffer_draw_glyph(const framebuffer_info_t *fb,
                            int x, int y, unsigned char ch,
                            uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph;

    if (!fb || fb->address == 0)
        return;
    if (x > INT_MAX - 7 || y > INT_MAX - 15)
        return;

    glyph = font8x16_glyph(ch);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (1u << col)) ? fg : bg;
            framebuffer_fill_rect(fb, x + col, y + row, 1, 1, color);
        }
    }
}

void framebuffer_draw_cursor(const framebuffer_info_t *fb,
                             int x, int y,
                             uint32_t fg, uint32_t shadow)
{
    static const uint16_t rows[12] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xF000, 0xD800, 0x8800, 0x0400,
    };

    if (x > INT_MAX - 7 || y > INT_MAX - 11)
        return;

    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            if (rows[row] & (0x8000u >> col))
                framebuffer_fill_rect(fb, x + col, y + row, 1, 1, fg);
        }
    }
    framebuffer_fill_rect(fb, x + 2, y + 2, 1, 1, shadow);
}

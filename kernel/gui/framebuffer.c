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
    int right;
    int bottom;

    if (!fb || w <= 0 || h <= 0)
        return out;
    if (x >= (int)fb->width || y >= (int)fb->height)
        return out;
    right = x + w;
    bottom = y + h;
    if (right <= 0 || bottom <= 0)
        return out;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (right > (int)fb->width)
        right = (int)fb->width;
    if (bottom > (int)fb->height)
        bottom = (int)fb->height;
    if (x >= right || y >= bottom)
        return out;
    out.x = x;
    out.y = y;
    out.w = right - x;
    out.h = bottom - y;
    return out;
}

void framebuffer_draw_rect_outline(const framebuffer_info_t *fb,
                                   int x, int y, int w, int h,
                                   uint32_t color)
{
    if (!fb || w <= 0 || h <= 0)
        return;
    framebuffer_fill_rect(fb, x, y, w, 1, color);
    framebuffer_fill_rect(fb, x, y + h - 1, w, 1, color);
    framebuffer_fill_rect(fb, x, y, 1, h, color);
    framebuffer_fill_rect(fb, x + w - 1, y, 1, h, color);
}

static void framebuffer_draw_glyph_clipped(const framebuffer_info_t *fb,
                                           const gui_pixel_rect_t *clip,
                                           int x, int y,
                                           unsigned char ch,
                                           uint32_t fg,
                                           uint32_t bg)
{
    const uint8_t *glyph;

    if (!fb || !clip || fb->address == 0)
        return;
    if (x > INT_MAX - 7 || y > INT_MAX - 15)
        return;

    glyph = font8x16_glyph(ch);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        int py = y + row;

        if (py < clip->y || py >= clip->y + clip->h)
            continue;
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            uint32_t color;

            if (px < clip->x || px >= clip->x + clip->w)
                continue;
            color = (bits & (1u << col)) ? fg : bg;
            framebuffer_fill_rect(fb, px, py, 1, 1, color);
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
    int col = 0;

    if (!fb || !clip || !text)
        return;
    bounded = framebuffer_clip_pixel_rect(fb, clip->x, clip->y, clip->w,
                                           clip->h);
    if (bounded.w <= 0 || bounded.h <= 0)
        return;
    while (text[col]) {
        framebuffer_draw_glyph_clipped(fb, &bounded,
                                       x + col * (int)GUI_FONT_W,
                                       y,
                                       (unsigned char)text[col],
                                       fg,
                                       bg);
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
    int thumb_h;
    int thumb_y;
    int max_top;
    int travel;

    if (!fb || w <= 0 || h <= 0)
        return;
    framebuffer_fill_rect(fb, x, y, w, h, track);
    if (total_rows <= 0 || visible_rows <= 0 || total_rows <= visible_rows)
        return;
    if (view_top < 0)
        view_top = 0;
    max_top = total_rows - visible_rows;
    if (view_top > max_top)
        view_top = max_top;
    thumb_h = (h * visible_rows) / total_rows;
    if (thumb_h < 8)
        thumb_h = h < 8 ? h : 8;
    if (thumb_h > h)
        thumb_h = h;
    travel = h - thumb_h;
    thumb_y = y;
    if (max_top > 0)
        thumb_y += (travel * view_top) / max_top;
    framebuffer_fill_rect(fb, x, thumb_y, w, thumb_h, thumb);
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

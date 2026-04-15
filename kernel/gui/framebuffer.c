#include "framebuffer.h"
#include "kstring.h"

static int rgb_mask_overlaps(uint32_t a_pos, uint32_t a_size,
                             uint32_t b_pos, uint32_t b_size)
{
    uint32_t a_end = a_pos + a_size;
    uint32_t b_end = b_pos + b_size;

    return a_pos < b_end && b_pos < a_end;
}

static uint32_t scale_color(uint8_t value, uint8_t mask_size)
{
    uint32_t max;

    if (mask_size >= 8)
        return value;
    if (mask_size == 0)
        return 0;
    max = (1u << mask_size) - 1u;
    return ((uint32_t)value * max + 127u) / 255u;
}

int framebuffer_info_from_multiboot(const multiboot_info_t *mbi,
                                    framebuffer_info_t *out)
{
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
    if (!fb || fb->address == 0 || w <= 0 || h <= 0)
        return;
    if (x >= (int)fb->width || y >= (int)fb->height)
        return;
    if (x + w <= 0 || y + h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int)fb->width)
        w = (int)fb->width - x;
    if (y + h > (int)fb->height)
        h = (int)fb->height - y;

    for (int row = 0; row < h; row++) {
        uint32_t *line = (uint32_t *)(fb->address +
                                      (uintptr_t)(y + row) * fb->pitch);
        for (int col = 0; col < w; col++)
            line[x + col] = color;
    }
}

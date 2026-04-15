#include "framebuffer.h"
#include "kstring.h"

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

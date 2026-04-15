#include "framebuffer.h"
#include "kstring.h"

int framebuffer_info_from_multiboot(const multiboot_info_t *mbi,
                                    framebuffer_info_t *out)
{
    if (!mbi || !out)
        return -1;
    if ((mbi->flags & MULTIBOOT_FLAG_FRAMEBUFFER) == 0)
        return -2;
    if (mbi->framebuffer_addr == 0)
        return -3;
    if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
        return -4;
    if (mbi->framebuffer_bpp != 32)
        return -5;
    if (mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0)
        return -6;
    if (mbi->framebuffer_pitch < mbi->framebuffer_width * 4u)
        return -7;
    if (mbi->framebuffer_width < GUI_FONT_W ||
        mbi->framebuffer_height < GUI_FONT_H)
        return -8;
    if (mbi->framebuffer_red_mask_size == 0 ||
        mbi->framebuffer_green_mask_size == 0 ||
        mbi->framebuffer_blue_mask_size == 0)
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

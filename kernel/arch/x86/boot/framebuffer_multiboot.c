#include "framebuffer_multiboot.h"
#include <limits.h>

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

	return framebuffer_info_from_rgb((uintptr_t)mbi->framebuffer_addr,
	                                 mbi->framebuffer_pitch,
	                                 mbi->framebuffer_width,
	                                 mbi->framebuffer_height,
	                                 mbi->framebuffer_bpp,
	                                 mbi->framebuffer_red_field_position,
	                                 mbi->framebuffer_red_mask_size,
	                                 mbi->framebuffer_green_field_position,
	                                 mbi->framebuffer_green_mask_size,
	                                 mbi->framebuffer_blue_field_position,
	                                 mbi->framebuffer_blue_mask_size,
	                                 out);
}

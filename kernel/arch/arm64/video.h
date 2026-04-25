#ifndef KERNEL_ARCH_ARM64_VIDEO_H
#define KERNEL_ARCH_ARM64_VIDEO_H

#include "framebuffer.h"
#include <stdint.h>

#ifndef DRUNIX_ARM64_VGA
#define DRUNIX_ARM64_VGA 0
#endif

#define ARM64_VIDEO_WIDTH 1024u
#define ARM64_VIDEO_HEIGHT 768u
#define ARM64_VIDEO_DEPTH 32u

int arm64_video_init(void);
int arm64_video_enabled(void);
framebuffer_info_t *arm64_video_framebuffer(void);
void arm64_video_console_write(const char *buf, uint32_t len);

#endif

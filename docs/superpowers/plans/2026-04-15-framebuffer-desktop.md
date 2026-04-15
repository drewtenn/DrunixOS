# Framebuffer Desktop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a hybrid `1024x768x32` framebuffer desktop mode with a larger `128x48` logical grid, RGB-rendered shell desktop, and pixel cursor, while preserving VGA fallback.

**Architecture:** Keep `gui_display_t` as the logical cell buffer and add a framebuffer presentation backend beside the existing VGA text presenter. Boot requests a Multiboot1 graphics mode, validates the provided framebuffer metadata, then configures either framebuffer presentation or the current `80x25` VGA fallback.

**Tech Stack:** C freestanding i386 kernel, NASM Multiboot1 header, GRUB2/QEMU, kernel KTEST, existing `kernel/gui` desktop/display modules.

---

## File Structure

- Modify `kernel/kernel-entry.asm`: request `1024x768x32` from Multiboot1.
- Modify `kernel/mm/pmm.h`: extend `multiboot_info_t` with framebuffer fields and constants.
- Create `kernel/gui/framebuffer.h`: framebuffer mode structs and primitive declarations.
- Create `kernel/gui/framebuffer.c`: mode validation, RGB packing, pixel primitives, glyph rendering, and cursor drawing.
- Create `kernel/gui/font8x16.h`: interface for expanded 8x16 bitmap font glyph lookup.
- Create `kernel/gui/font8x16.c`: compiled-in 8x8 printable ASCII bitmap data expanded to 8x16 rows.
- Modify `kernel/gui/display.h`: add runtime presentation target metadata and framebuffer present API.
- Modify `kernel/gui/display.c`: initialize dynamic cell display and present cells through framebuffer.
- Modify `kernel/gui/desktop.h`: track pointer mode and pixel coordinates when framebuffer is active.
- Modify `kernel/gui/desktop.c`: adapt layout for larger grids and draw pixel cursor in framebuffer mode.
- Modify `kernel/kernel.c`: select framebuffer/VGA mode at boot and allocate the logical cell buffer.
- Modify `kernel/test/test_desktop.c`: add framebuffer, layout, and cursor tests.
- Modify `Makefile`: add new GUI object files.
- Modify `iso/boot/grub/grub.cfg` during Task 9 only when the Task 9 smoke log proves GRUB ignored the Multiboot header video request.

## Task 0: Confirm Feature Branch And Baseline

**Files:**
- No source files changed.

- [ ] **Step 1: Confirm the isolated branch**

Run:

```bash
git status --short
git branch --show-current
```

Expected: current branch is `feature/framebuffer-desktop`. `git status --short` may show `.superpowers/` from brainstorming; leave it uncommitted.

- [ ] **Step 2: Verify baseline**

Run:

```bash
make KTEST=1 kernel
make disk
make test
```

Expected: `make KTEST=1 kernel` and `make disk` exit 0. `make test` launches QEMU; after several seconds, manually stop it with:

```bash
pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"
```

Then verify:

```bash
rg -n "KTEST|FAIL|desktop|BOOT|framebuffer|panic|PANIC" debugcon.log
```

Expected: all KTEST suites pass, desktop passes the current test count, and boot reaches ring 3.

## Task 1: Multiboot Framebuffer Metadata

**Files:**
- Modify: `kernel/kernel-entry.asm`
- Modify: `kernel/mm/pmm.h`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing tests for framebuffer metadata validation shape**

Add these declarations near the top of `kernel/test/test_desktop.c` after the existing externs:

```c
extern int framebuffer_info_from_multiboot(const multiboot_info_t *mbi,
                                           framebuffer_info_t *out);
```

Add a test that builds a synthetic Multiboot info record:

```c
static void test_framebuffer_info_accepts_1024_768_32_rgb(ktest_case_t *tc)
{
    multiboot_info_t mbi;
    framebuffer_info_t info;

    k_memset(&mbi, 0, sizeof(mbi));
    k_memset(&info, 0, sizeof(info));
    mbi.flags = MULTIBOOT_FLAG_FRAMEBUFFER;
    mbi.framebuffer_addr = 0xE0000000ull;
    mbi.framebuffer_pitch = 1024u * 4u;
    mbi.framebuffer_width = 1024u;
    mbi.framebuffer_height = 768u;
    mbi.framebuffer_bpp = 32u;
    mbi.framebuffer_type = MULTIBOOT_FRAMEBUFFER_TYPE_RGB;
    mbi.framebuffer_red_field_position = 16u;
    mbi.framebuffer_red_mask_size = 8u;
    mbi.framebuffer_green_field_position = 8u;
    mbi.framebuffer_green_mask_size = 8u;
    mbi.framebuffer_blue_field_position = 0u;
    mbi.framebuffer_blue_mask_size = 8u;

    KTEST_EXPECT_EQ(tc, framebuffer_info_from_multiboot(&mbi, &info), 0);
    KTEST_EXPECT_EQ(tc, info.width, 1024u);
    KTEST_EXPECT_EQ(tc, info.height, 768u);
    KTEST_EXPECT_EQ(tc, info.bpp, 32u);
    KTEST_EXPECT_EQ(tc, info.cell_cols, 128u);
    KTEST_EXPECT_EQ(tc, info.cell_rows, 48u);
}
```

Add it to `desktop_cases`.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails because `framebuffer_info_t`, `MULTIBOOT_FLAG_FRAMEBUFFER`, and `framebuffer_info_from_multiboot()` are not defined.

- [ ] **Step 3: Extend Multiboot structures**

Modify `kernel/mm/pmm.h` so `multiboot_info_t` includes the Multiboot1 fields through framebuffer metadata:

```c
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint8_t  syms[16];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  framebuffer_red_field_position;
    uint8_t  framebuffer_red_mask_size;
    uint8_t  framebuffer_green_field_position;
    uint8_t  framebuffer_green_mask_size;
    uint8_t  framebuffer_blue_field_position;
    uint8_t  framebuffer_blue_mask_size;
} __attribute__((packed)) multiboot_info_t;

#define MULTIBOOT_FLAG_MMAP        (1u << 6)
#define MULTIBOOT_FLAG_FRAMEBUFFER (1u << 12)
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB 1u
```

- [ ] **Step 4: Request framebuffer mode in the Multiboot header**

Modify `kernel/kernel-entry.asm`:

```asm
    MULTIBOOT_FLAGS    equ 0x04
    MULTIBOOT_CHECKSUM equ -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

    dd MULTIBOOT_MAGIC
    dd MULTIBOOT_FLAGS
    dd MULTIBOOT_CHECKSUM
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd 1024
    dd 768
    dd 32
```

This uses Multiboot1 header flag bit 2 and appends the video mode request fields.

- [ ] **Step 5: Add framebuffer info validation API skeleton**

Create `kernel/gui/framebuffer.h`:

```c
#ifndef GUI_FRAMEBUFFER_H
#define GUI_FRAMEBUFFER_H

#include "pmm.h"
#include <stdint.h>

#define GUI_FONT_W 8u
#define GUI_FONT_H 16u

typedef struct {
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

#endif
```

Create `kernel/gui/framebuffer.c`:

```c
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
```

- [ ] **Step 6: Add new object to Makefile**

Add `kernel/gui/framebuffer.o` to the kernel object list near `kernel/gui/display.o`.

- [ ] **Step 7: Run test to verify it passes**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0.

- [ ] **Step 8: Commit**

```bash
git add kernel/kernel-entry.asm kernel/mm/pmm.h kernel/gui/framebuffer.h kernel/gui/framebuffer.c kernel/test/test_desktop.c Makefile
git commit -m "feat: read multiboot framebuffer info"
```

## Task 2: Framebuffer Pixel Primitives

**Files:**
- Modify: `kernel/gui/framebuffer.h`
- Modify: `kernel/gui/framebuffer.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing tests for RGB packing and clipped fill**

Add:

```c
static void test_framebuffer_pack_rgb_uses_mask_positions(ktest_case_t *tc)
{
    framebuffer_info_t info;

    k_memset(&info, 0, sizeof(info));
    info.red_pos = 16;
    info.red_size = 8;
    info.green_pos = 8;
    info.green_size = 8;
    info.blue_pos = 0;
    info.blue_size = 8;

    KTEST_EXPECT_EQ(tc, framebuffer_pack_rgb(&info, 0x12, 0x34, 0x56),
                    0x00123456u);
}

static void test_framebuffer_fill_rect_clips_to_bounds(ktest_case_t *tc)
{
    uint32_t pixels[4 * 4];
    framebuffer_info_t info;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&info, 0, sizeof(info));
    info.address = (uintptr_t)pixels;
    info.pitch = 4u * sizeof(uint32_t);
    info.width = 4;
    info.height = 4;

    framebuffer_fill_rect(&info, -1, 1, 3, 2, 0xAABBCCDDu);

    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 0], 0xAABBCCDDu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 1], 0xAABBCCDDu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 2], 0u);
    KTEST_EXPECT_EQ(tc, pixels[0], 0u);
}
```

Add both to `desktop_cases`.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails for missing `framebuffer_pack_rgb()` and `framebuffer_fill_rect()`.

- [ ] **Step 3: Implement primitives**

Add declarations to `kernel/gui/framebuffer.h`:

```c
uint32_t framebuffer_pack_rgb(const framebuffer_info_t *fb,
                              uint8_t r, uint8_t g, uint8_t b);
void framebuffer_fill_rect(const framebuffer_info_t *fb,
                           int x, int y, int w, int h,
                           uint32_t color);
```

Add implementation:

```c
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
        uint32_t *line = (uint32_t *)(fb->address + (uintptr_t)(y + row) * fb->pitch);
        for (int col = 0; col < w; col++)
            line[x + col] = color;
    }
}
```

- [ ] **Step 4: Run tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0.

- [ ] **Step 5: Commit**

```bash
git add kernel/gui/framebuffer.h kernel/gui/framebuffer.c kernel/test/test_desktop.c
git commit -m "feat: add framebuffer pixel primitives"
```

## Task 3: Bitmap Font And Glyph Rendering

**Files:**
- Create: `kernel/gui/font8x16.h`
- Create: `kernel/gui/font8x16.c`
- Modify: `kernel/gui/framebuffer.h`
- Modify: `kernel/gui/framebuffer.c`
- Modify: `Makefile`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing glyph render test**

Add:

```c
static void test_framebuffer_draw_glyph_writes_foreground_pixels(ktest_case_t *tc)
{
    uint32_t pixels[16 * 16];
    framebuffer_info_t info;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&info, 0, sizeof(info));
    info.address = (uintptr_t)pixels;
    info.pitch = 16u * sizeof(uint32_t);
    info.width = 16;
    info.height = 16;

    framebuffer_draw_glyph(&info, 0, 0, 'A', 0x00FFFFFFu, 0x00000000u);

    KTEST_EXPECT_NE(tc, pixels[0], pixels[15 * 16 + 15]);
}
```

Add to `desktop_cases`.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails for missing `framebuffer_draw_glyph()`.

- [ ] **Step 3: Add font interface**

Create `kernel/gui/font8x16.h`:

```c
#ifndef GUI_FONT8X16_H
#define GUI_FONT8X16_H

#include <stdint.h>

const uint8_t *font8x16_glyph(unsigned char ch);

#endif
```

Create `kernel/gui/font8x16.c` with compiled-in 8x8 glyph rows for printable ASCII and expand each row to two rows at lookup time. Use the public-domain `font8x8_basic` ASCII table from the `dhepper/font8x8` project and keep this source note at the top of the file:

```c
/*
 * Printable ASCII bitmap font based on the public-domain font8x8_basic table
 * from https://github.com/dhepper/font8x8. Each 8-row glyph is expanded to
 * 16 rows by duplicating each source row during lookup.
 */
```

Build the `font8x8_basic[96][8]` initializer from entries `0x20..0x7f` of the source table, preserving row byte order. Verify the generated initializer has 96 opening row braces:

```bash
rg -n "static const uint8_t font8x8_basic\\[96\\]\\[8\\]" kernel/gui/font8x16.c
python3 - <<'PY'
import re
text = open("kernel/gui/font8x16.c", encoding="utf-8").read()
body = re.search(r"font8x8_basic\\[96\\]\\[8\\]\\s*=\\s*\\{(.*?)\\};", text, re.S)
rows = re.findall(r"\\{\\s*0x[0-9a-fA-F]{2}", body.group(1) if body else "")
if len(rows) != 96:
    raise SystemExit(f"expected 96 font rows, found {len(rows)}")
PY
```

The file must expose:

```c
#include "font8x16.h"
#include "kstring.h"

static const uint8_t fallback_glyph[16] = {
    0x00, 0x7E, 0x42, 0x5A, 0x5A, 0x42, 0x7E, 0x00,
    0x00, 0x7E, 0x42, 0x5A, 0x5A, 0x42, 0x7E, 0x00,
};

static uint8_t expanded_glyph[16];

static const uint8_t font8x8_basic[96][8] = {
    /* 96 generated entries from font8x8_basic[0x20..0x7f]. */
};

const uint8_t *font8x16_glyph(unsigned char ch)
{
    const uint8_t *src;

    if (ch < 0x20 || ch > 0x7f)
        return fallback_glyph;

    src = font8x8_basic[ch - 0x20];
    for (int row = 0; row < 8; row++) {
        expanded_glyph[row * 2] = src[row];
        expanded_glyph[row * 2 + 1] = src[row];
    }
    return expanded_glyph;
}
```

The committed `font8x8_basic` table must contain all 96 printable ASCII entries; the first entry for space is eight zero bytes, `A` has non-zero rows, and `~` has non-zero rows. Do not merge Task 3 with a truncated table.

- [ ] **Step 4: Add glyph rendering**

Add declaration:

```c
void framebuffer_draw_glyph(const framebuffer_info_t *fb,
                            int x, int y, unsigned char ch,
                            uint32_t fg, uint32_t bg);
```

Add implementation:

```c
#include "font8x16.h"

void framebuffer_draw_glyph(const framebuffer_info_t *fb,
                            int x, int y, unsigned char ch,
                            uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph;

    if (!fb || fb->address == 0)
        return;

    glyph = font8x16_glyph(ch);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80u >> col)) ? fg : bg;
            framebuffer_fill_rect(fb, x + col, y + row, 1, 1, color);
        }
    }
}
```

- [ ] **Step 5: Add object to Makefile**

Add `kernel/gui/font8x16.o` to the kernel object list.

- [ ] **Step 6: Run tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0.

- [ ] **Step 7: Commit**

```bash
git add kernel/gui/font8x16.h kernel/gui/font8x16.c kernel/gui/framebuffer.h kernel/gui/framebuffer.c kernel/test/test_desktop.c Makefile
git commit -m "feat: add framebuffer bitmap font rendering"
```

## Task 4: Framebuffer Cell Presentation

**Files:**
- Modify: `kernel/gui/framebuffer.h`
- Modify: `kernel/gui/framebuffer.c`
- Modify: `kernel/gui/display.h`
- Modify: `kernel/gui/display.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing cell presentation test**

Add:

```c
static void test_gui_display_presents_cells_to_framebuffer(ktest_case_t *tc)
{
    gui_cell_t cells[2];
    uint32_t pixels[8 * 16 * 2];
    gui_display_t display;
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 16u * sizeof(uint32_t);
    fb.width = 16;
    fb.height = 16;
    fb.red_pos = 16;
    fb.red_size = 8;
    fb.green_pos = 8;
    fb.green_size = 8;
    fb.blue_pos = 0;
    fb.blue_size = 8;

    gui_display_init(&display, cells, 2, 1, 0x0f);
    gui_display_draw_text(&display, 0, 0, 1, "A", 0x0f);

    gui_display_present_to_framebuffer(&display, &fb);

    KTEST_EXPECT_NE(tc, pixels[0], 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails for missing `gui_display_present_to_framebuffer()`.

- [ ] **Step 3: Add display-local VGA attribute to RGB palette**

Add this private helper near the top of `kernel/gui/display.c`, after the includes:

```c
static void gui_display_vga_color(uint8_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t palette[16][3] = {
        {0x00,0x00,0x00}, {0x00,0x00,0xaa}, {0x00,0xaa,0x00}, {0x00,0xaa,0xaa},
        {0xaa,0x00,0x00}, {0xaa,0x00,0xaa}, {0xaa,0x55,0x00}, {0xaa,0xaa,0xaa},
        {0x55,0x55,0x55}, {0x55,0x55,0xff}, {0x55,0xff,0x55}, {0x55,0xff,0xff},
        {0xff,0x55,0x55}, {0xff,0x55,0xff}, {0xff,0xff,0x55}, {0xff,0xff,0xff},
    };
    *r = palette[color & 0x0f][0];
    *g = palette[color & 0x0f][1];
    *b = palette[color & 0x0f][2];
}
```

- [ ] **Step 4: Add display framebuffer presentation API**

Add to `kernel/gui/display.h`:

```c
struct framebuffer_info;
void gui_display_present_to_framebuffer(const gui_display_t *display,
                                        const struct framebuffer_info *fb);
```

Define `typedef struct framebuffer_info framebuffer_info_t;` in `framebuffer.h` by naming the struct:

```c
typedef struct framebuffer_info {
    ...
} framebuffer_info_t;
```

Add to `kernel/gui/display.c`:

```c
#include "framebuffer.h"

void gui_display_present_to_framebuffer(const gui_display_t *display,
                                        const framebuffer_info_t *fb)
{
    uint8_t r, g, b;

    if (!display || !fb)
        return;
    if (!display->cells || display->cols <= 0 || display->rows <= 0)
        return;

    for (int row = 0; row < display->rows; row++) {
        for (int col = 0; col < display->cols; col++) {
            const gui_cell_t *cell = &display->cells[row * display->cols + col];
            gui_display_vga_color(cell->attr & 0x0f, &r, &g, &b);
            uint32_t fg = framebuffer_pack_rgb(fb, r, g, b);
            gui_display_vga_color((cell->attr >> 4) & 0x0f, &r, &g, &b);
            uint32_t bg = framebuffer_pack_rgb(fb, r, g, b);
            framebuffer_draw_glyph(fb, col * 8, row * 16,
                                   (unsigned char)cell->ch, fg, bg);
        }
    }
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0.

- [ ] **Step 6: Commit**

```bash
git add kernel/gui/framebuffer.h kernel/gui/framebuffer.c kernel/gui/display.h kernel/gui/display.c kernel/test/test_desktop.c
git commit -m "feat: present gui cells to framebuffer"
```

## Task 5: Dynamic Desktop Display Initialization

**Files:**
- Modify: `kernel/kernel.c`
- Modify: `kernel/gui/display.h`
- Modify: `kernel/gui/desktop.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing test for `128x48` layout**

Add:

```c
static gui_cell_t large_desktop_cells[128 * 48];

static void test_desktop_layout_scales_to_framebuffer_grid(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, large_desktop_cells, 128, 48, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_EQ(tc, desktop.taskbar.y, 47);
    KTEST_EXPECT_GE(tc, desktop.shell_rect.w, 100);
    KTEST_EXPECT_GE(tc, desktop.shell_rect.h, 30);
    KTEST_EXPECT_GE(tc, desktop.shell_content.w, 98);
    KTEST_EXPECT_GE(tc, desktop.shell_content.h, 28);
}
```

- [ ] **Step 2: Run test**

Run:

```bash
make KTEST=1 kernel
```

Expected: if it already passes, keep it as regression coverage. If it fails, adjust `desktop_layout()`.

- [ ] **Step 3: Make desktop layout robust for large grids**

In `desktop_layout()`, keep the taskbar one cell high at the bottom, scale the shell window using margins based on grid size, and keep launcher below/near taskbar without overlapping shell:

```c
int margin_x = desktop->display->cols >= 100 ? 10 : 6;
int margin_top = desktop->display->rows >= 40 ? 4 : 3;
int taskbar_gap = desktop->display->rows >= 40 ? 8 : 6;

desktop->shell_rect.x = margin_x;
desktop->shell_rect.y = margin_top;
desktop->shell_rect.w = desktop->display->cols - margin_x * 2;
desktop->shell_rect.h = desktop->display->rows - margin_top - taskbar_gap;
```

Clamp minimum widths/heights so `80x25` keeps current behavior.

- [ ] **Step 4: Add boot display cell buffers**

In `kernel/kernel.c`, replace the single `boot_desktop_cells[MAX_ROWS * MAX_COLS]` with both text and framebuffer-capable buffers:

```c
#define FB_REQUEST_WIDTH 1024
#define FB_REQUEST_HEIGHT 768
#define FB_CELL_COLS (FB_REQUEST_WIDTH / 8)
#define FB_CELL_ROWS (FB_REQUEST_HEIGHT / 16)

static gui_cell_t boot_vga_cells[MAX_ROWS * MAX_COLS];
static gui_cell_t boot_fb_cells[FB_CELL_COLS * FB_CELL_ROWS];
static framebuffer_info_t boot_framebuffer;
```

Select the cell buffer after framebuffer validation:

```c
int have_fb = framebuffer_info_from_multiboot(mbi, &boot_framebuffer) == 0;
gui_cell_t *cells = have_fb ? boot_fb_cells : boot_vga_cells;
int cols = have_fb ? (int)boot_framebuffer.cell_cols : MAX_COLS;
int rows = have_fb ? (int)boot_framebuffer.cell_rows : MAX_ROWS;
gui_display_init(&boot_display, cells, cols, rows, WHITE_ON_BLACK);
```

- [ ] **Step 5: Run tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0.

- [ ] **Step 6: Commit**

```bash
git add kernel/kernel.c kernel/gui/desktop.c kernel/test/test_desktop.c
git commit -m "feat: support framebuffer-sized desktop grid"
```

## Task 6: Presentation Target Selection

**Files:**
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/kernel.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing test for framebuffer presentation target**

Add:

```c
static void test_desktop_can_use_framebuffer_presentation_target(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t pixels[16 * 16];

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 16u * sizeof(uint32_t);
    fb.width = 16;
    fb.height = 16;
    fb.red_pos = 16;
    fb.red_size = 8;
    fb.green_pos = 8;
    fb.green_size = 8;
    fb.blue_pos = 0;
    fb.blue_size = 8;

    gui_display_init(&display, desktop_cells, 2, 1, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_render(&desktop);

    KTEST_EXPECT_NE(tc, pixels[0], 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails for missing `desktop_set_framebuffer_target()`.

- [ ] **Step 3: Add desktop presentation mode**

Add to `desktop_state_t`:

```c
const framebuffer_info_t *framebuffer;
int framebuffer_enabled;
```

Add declaration:

```c
void desktop_set_framebuffer_target(desktop_state_t *desktop,
                                    const framebuffer_info_t *framebuffer);
```

Implementation:

```c
void desktop_set_framebuffer_target(desktop_state_t *desktop,
                                    const framebuffer_info_t *framebuffer)
{
    if (!desktop)
        return;
    desktop->framebuffer = framebuffer;
    desktop->framebuffer_enabled = framebuffer != 0;
}
```

- [ ] **Step 4: Present through selected target**

At the end of `desktop_render()`:

```c
if (desktop->framebuffer_enabled && desktop->framebuffer) {
    gui_display_present_to_framebuffer(desktop->display, desktop->framebuffer);
} else if (desktop->video_address) {
    gui_display_present_to_vga(desktop->display, desktop->video_address);
}
```

- [ ] **Step 5: Wire boot selection**

In `kernel/kernel.c`:

```c
if (have_fb) {
    desktop_set_framebuffer_target(&boot_desktop, &boot_framebuffer);
    klog("BOOT", "desktop framebuffer enabled");
} else {
    desktop_set_presentation_target(&boot_desktop, VIDEO_ADDRESS);
    klog("BOOT", "desktop VGA fallback enabled");
}
```

- [ ] **Step 6: Run tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0.

- [ ] **Step 7: Commit**

```bash
git add kernel/gui/desktop.h kernel/gui/desktop.c kernel/kernel.c kernel/test/test_desktop.c
git commit -m "feat: select framebuffer desktop presentation"
```

## Task 7: Pixel Arrow Cursor

**Files:**
- Modify: `kernel/gui/framebuffer.h`
- Modify: `kernel/gui/framebuffer.c`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/drivers/mouse.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing cursor test**

Add:

```c
static void test_framebuffer_draws_pixel_arrow_cursor(ktest_case_t *tc)
{
    uint32_t pixels[16 * 16];
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 16u * sizeof(uint32_t);
    fb.width = 16;
    fb.height = 16;

    framebuffer_draw_cursor(&fb, 2, 2, 0x00FFFFFFu, 0x00000000u);

    KTEST_EXPECT_EQ(tc, pixels[2 * 16 + 2], 0x00FFFFFFu);
    KTEST_EXPECT_EQ(tc, pixels[3 * 16 + 2], 0x00FFFFFFu);
    KTEST_EXPECT_EQ(tc, pixels[3 * 16 + 3], 0x00FFFFFFu);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails for missing `framebuffer_draw_cursor()`.

- [ ] **Step 3: Add framebuffer cursor primitive**

Declaration:

```c
void framebuffer_draw_cursor(const framebuffer_info_t *fb,
                             int x, int y,
                             uint32_t fg,
                             uint32_t shadow);
```

Implementation:

```c
void framebuffer_draw_cursor(const framebuffer_info_t *fb,
                             int x, int y,
                             uint32_t fg,
                             uint32_t shadow)
{
    static const uint16_t rows[12] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xF000, 0xD800, 0x8800, 0x0400,
    };

    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            if (rows[row] & (0x8000u >> col))
                framebuffer_fill_rect(fb, x + col, y + row, 1, 1, fg);
        }
    }
    framebuffer_fill_rect(fb, x + 2, y + 2, 1, 1, shadow);
}
```

- [ ] **Step 4: Store pixel pointer coordinates**

Extend `desktop_state_t`:

```c
int pointer_pixel_x;
int pointer_pixel_y;
```

In framebuffer mode, `desktop_handle_pointer()` should accept both cell and pixel coordinates. Keep `ev->x`/`ev->y` as cells for hit testing, and add `pixel_x`/`pixel_y` to `desktop_pointer_event_t`:

```c
int pixel_x;
int pixel_y;
```

In `mouse_update_pointer()`, set:

```c
ev->pixel_x = g_pointer_pixel_x;
ev->pixel_y = g_pointer_pixel_y;
ev->x = g_pointer_pixel_x / 8;
ev->y = g_pointer_pixel_y / 16;
```

Clamp pixel coordinates to framebuffer dimensions when framebuffer is active and to `cols * 8` / `rows * 16` otherwise.

- [ ] **Step 5: Draw cursor after framebuffer presentation**

In framebuffer mode, draw the pixel cursor after presenting the cell grid:

```c
if (desktop->framebuffer_enabled && desktop->framebuffer) {
    gui_display_present_to_framebuffer(desktop->display, desktop->framebuffer);
    framebuffer_draw_cursor(desktop->framebuffer,
                            desktop->pointer_pixel_x,
                            desktop->pointer_pixel_y,
                            framebuffer_pack_rgb(desktop->framebuffer, 255, 255, 255),
                            framebuffer_pack_rgb(desktop->framebuffer, 0, 0, 0));
}
```

In VGA mode, keep the existing cell `^` pointer path.

- [ ] **Step 6: Run tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0 and desktop pointer tests pass.

- [ ] **Step 7: Commit**

```bash
git add kernel/gui/framebuffer.h kernel/gui/framebuffer.c kernel/gui/desktop.h kernel/gui/desktop.c kernel/drivers/mouse.c kernel/test/test_desktop.c
git commit -m "feat: draw framebuffer pixel cursor"
```

## Task 8: Polished Framebuffer Theme

**Files:**
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/gui/framebuffer.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write theme regression test**

Add a test that verifies desktop render still writes taskbar/menu cells on a large framebuffer grid:

```c
static void test_framebuffer_grid_desktop_renders_taskbar_and_shell_title(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, large_desktop_cells, 128, 48, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 2, 47).ch, 'M');
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_rect.x + 2,
                                        desktop.shell_rect.y).ch,
                    'S');
}
```

- [ ] **Step 2: Run test**

Run:

```bash
make KTEST=1 kernel
```

Expected: pass or expose layout regression.

- [ ] **Step 3: Add framebuffer-aware theme attrs**

Keep `gui_cell_t.attr` as the theme carrier. Update `desktop_render()` to use stable attrs for:

```c
#define DESKTOP_ATTR_BACKGROUND 0x1f
#define DESKTOP_ATTR_TASKBAR    0x70
#define DESKTOP_ATTR_WINDOW     0x1e
#define DESKTOP_ATTR_TITLE      0x70
#define DESKTOP_ATTR_LAUNCHER   0x70
```

Use these named constants inside `desktop_render()`. The framebuffer palette can map them to richer RGB colors while VGA remains readable.

- [ ] **Step 4: Tune framebuffer palette**

Replace the `palette` values in `gui_display_vga_color()` with this richer RGB table while preserving the 16-color attribute contract:

```c
static const uint8_t palette[16][3] = {
    {0x06,0x08,0x12}, {0x16,0x2a,0x4f}, {0x1f,0x6f,0x54}, {0x27,0x8d,0x95},
    {0x84,0x2f,0x3a}, {0x7c,0x3f,0x8f}, {0xb8,0x74,0x2a}, {0xc8,0xd1,0xd9},
    {0x4d,0x5b,0x6a}, {0x4a,0x78,0xc2}, {0x67,0xc5,0x8f}, {0x6f,0xd6,0xd2},
    {0xe0,0x6c,0x75}, {0xc6,0x78,0xdd}, {0xf2,0xc9,0x4c}, {0xf6,0xf1,0xde},
};
```

- [ ] **Step 5: Run tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: build exits 0.

- [ ] **Step 6: Commit**

```bash
git add kernel/gui/desktop.c kernel/gui/framebuffer.c kernel/test/test_desktop.c
git commit -m "style: polish framebuffer desktop theme"
```

## Task 9: Boot Smoke Verification And Documentation

**Files:**
- Modify: `docs/superpowers/specs/2026-04-15-framebuffer-desktop-design.md` when implementation intentionally diverges from the saved design.
- Modify: book docs only after an explicit docs request from the user.

- [ ] **Step 1: Run full verification**

Run:

```bash
make KTEST=1 kernel
make disk
make test
```

Manually stop QEMU:

```bash
pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"
```

Inspect:

```bash
rg -n "KTEST|FAIL|framebuffer|desktop|BOOT|panic|PANIC" debugcon.log
```

Expected:

- KTEST begins and ends
- all suites pass
- desktop suite includes framebuffer tests
- boot log reports either framebuffer desktop enabled or VGA fallback enabled
- boot reaches ring 3 shell launch

- [ ] **Step 2: Run normal boot smoke**

Run:

```bash
make kernel
make run
```

Manually stop QEMU after a short smoke window:

```bash
pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"
```

Expected: QEMU boots without panic. In framebuffer-capable QEMU/GRUB, the desktop appears in graphics mode with a larger shell area and pixel cursor. If framebuffer negotiation fails, VGA fallback appears and boot logs explain fallback.

- [ ] **Step 3: Check repository state**

Run:

```bash
git status --short
git diff --check
```

Expected: no unstaged source changes except intentional docs if any; `git diff --check` prints no whitespace errors.

- [ ] **Step 4: Commit final docs or verification adjustment**

If docs changed:

```bash
git add docs/superpowers/specs/2026-04-15-framebuffer-desktop-design.md
git commit -m "docs: update framebuffer desktop implementation notes"
```

When docs remain unchanged, skip this commit.

## Final Review Checklist

- [ ] Framebuffer boot request exists in Multiboot header.
- [ ] Kernel validates framebuffer metadata before use.
- [ ] VGA fallback still works.
- [ ] `1024x768x32` produces a `128x48` logical grid.
- [ ] Framebuffer renderer clips safely.
- [ ] RGB packing respects mask positions.
- [ ] Bitmap glyph rendering is readable.
- [ ] Shell output still routes through the existing desktop console path.
- [ ] Pixel cursor is drawn only in framebuffer mode.
- [ ] Cell `^` cursor still works in VGA fallback.
- [ ] QEMU KTEST boot passes.
- [ ] Normal QEMU boot smoke passes.

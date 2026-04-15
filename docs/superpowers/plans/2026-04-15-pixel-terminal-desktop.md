# Pixel Terminal Desktop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a pixel-native framebuffer desktop path where the shell runs inside a terminal surface with padding, underline cursor, keyboard scrollback, and a visible scrollbar, while VGA fallback keeps the existing cell path.

**Architecture:** Add small framebuffer pixel primitives, introduce a reusable pixel surface boundary, move terminal state out of `desktop.c` into a focused terminal module, and make framebuffer mode render desktop chrome and terminal pixels directly. Keep existing shell, TTY, process ownership, and VGA fallback behavior.

**Tech Stack:** Freestanding C kernel, Multiboot framebuffer, in-kernel KTEST suite, Makefile build, QEMU smoke verification.

---

## File Structure

- Create `kernel/gui/pixel.h`: shared pixel rectangle, color theme, and bounded surface types.
- Modify `kernel/gui/framebuffer.h`: expose rectangle outline, clipped text, and scrollbar drawing helpers.
- Modify `kernel/gui/framebuffer.c`: implement the new pixel primitives using existing clipping-safe framebuffer writes.
- Create `kernel/gui/terminal.h`: terminal surface API for allocation, byte writes, clear, scroll view, geometry, and rendering.
- Create `kernel/gui/terminal.c`: shell terminal model, ANSI state, live rows, retained history, dirty regions, and pixel rendering.
- Modify `kernel/gui/desktop.h`: replace embedded shell cell/cursor fields with a `gui_terminal_t` and pixel rect fields.
- Modify `kernel/gui/desktop.c`: keep desktop ownership and input routing, but delegate framebuffer rendering to pixel primitives and the terminal surface.
- Modify `kernel/proc/syscall.c`: route `SYS_SCROLL_UP` and `SYS_SCROLL_DOWN` to the desktop terminal when desktop mode is active.
- Modify `kernel/test/test_desktop.c`: add pixel primitive, terminal, desktop framebuffer, and syscall scrollback tests.
- Modify `Makefile`: add `kernel/gui/terminal.o` to `KOBJS`.
- Modify `docs/ch03-kernel-entry.md` and `docs/ch22-shell.md`: update the runtime story once code changes land.

## Task 1: Pixel Renderer Primitives

**Files:**
- Create: `kernel/gui/pixel.h`
- Modify: `kernel/gui/framebuffer.h`
- Modify: `kernel/gui/framebuffer.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing tests for outline, clipped text, and scrollbar geometry**

Add these tests near the existing framebuffer tests in `kernel/test/test_desktop.c`:

```c
static void test_framebuffer_draw_rect_outline_clips_to_bounds(ktest_case_t *tc)
{
    uint32_t pixels[6 * 6];
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 6u * sizeof(uint32_t);
    fb.width = 6;
    fb.height = 6;

    framebuffer_draw_rect_outline(&fb, -1, 1, 4, 4, 0x00ABCDEFu);

    KTEST_EXPECT_EQ(tc, pixels[1 * 6 + 0], 0x00ABCDEFu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 6 + 2], 0x00ABCDEFu);
    KTEST_EXPECT_EQ(tc, pixels[3 * 6 + 0], 0x00ABCDEFu);
    KTEST_EXPECT_EQ(tc, pixels[4 * 6 + 0], 0u);
}

static void test_framebuffer_draw_text_clipped_honors_pixel_clip(ktest_case_t *tc)
{
    static uint32_t pixels[32 * 20];
    framebuffer_info_t fb;
    gui_pixel_rect_t clip = { 8, 0, 8, 16 };

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 32u * sizeof(uint32_t);
    fb.width = 32;
    fb.height = 20;

    framebuffer_draw_text_clipped(&fb, &clip, 0, 0, "AB",
                                  0x00FFFFFFu, 0x00112233u);

    KTEST_EXPECT_EQ(tc, pixels[0 * 32 + 0], 0u);
    KTEST_EXPECT_NE(tc, pixels[0 * 32 + 8], 0u);
    KTEST_EXPECT_EQ(tc, pixels[0 * 32 + 16], 0u);
}

static void test_framebuffer_draw_scrollbar_places_thumb(ktest_case_t *tc)
{
    uint32_t pixels[8 * 40];
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 8u * sizeof(uint32_t);
    fb.width = 8;
    fb.height = 40;

    framebuffer_draw_scrollbar(&fb, 2, 4, 4, 32, 100, 25, 50,
                               0x00010101u, 0x00EEEEEEu);

    KTEST_EXPECT_EQ(tc, pixels[4 * 8 + 2], 0x00010101u);
    KTEST_EXPECT_EQ(tc, pixels[20 * 8 + 2], 0x00EEEEEEu);
    KTEST_EXPECT_EQ(tc, pixels[35 * 8 + 5], 0x00010101u);
}
```

Add the cases to `desktop_cases[]` immediately after `test_framebuffer_fill_rect_handles_large_dimensions`:

```c
KTEST_CASE(test_framebuffer_draw_rect_outline_clips_to_bounds),
KTEST_CASE(test_framebuffer_draw_text_clipped_honors_pixel_clip),
KTEST_CASE(test_framebuffer_draw_scrollbar_places_thumb),
```

- [ ] **Step 2: Run the failing tests by building the KTEST kernel**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails because `gui_pixel_rect_t`, `framebuffer_draw_rect_outline`, `framebuffer_draw_text_clipped`, and `framebuffer_draw_scrollbar` are not defined.

- [ ] **Step 3: Add pixel shared types**

Create `kernel/gui/pixel.h`. Do not include `framebuffer.h`; forward-declare the framebuffer struct so `framebuffer.h` can include this header without a cycle:

```c
#ifndef GUI_PIXEL_H
#define GUI_PIXEL_H

#include <stdint.h>

struct framebuffer_info;

typedef struct gui_pixel_rect {
    int x;
    int y;
    int w;
    int h;
} gui_pixel_rect_t;

typedef struct {
    const struct framebuffer_info *fb;
    gui_pixel_rect_t clip;
} gui_pixel_surface_t;

typedef struct {
    uint32_t desktop_bg;
    uint32_t taskbar_bg;
    uint32_t taskbar_fg;
    uint32_t window_bg;
    uint32_t window_border;
    uint32_t title_bg;
    uint32_t title_fg;
    uint32_t terminal_bg;
    uint32_t terminal_fg;
    uint32_t terminal_dim;
    uint32_t terminal_cursor;
    uint32_t scrollbar_track;
    uint32_t scrollbar_thumb;
} gui_pixel_theme_t;

#endif /* GUI_PIXEL_H */
```

- [ ] **Step 4: Implement pixel clipping**

Add this `static` helper to `kernel/gui/framebuffer.c` above `framebuffer_draw_rect_outline()`. When `terminal.c` needs rectangle clipping, add a second `static` helper there with the same body and the name `terminal_clip_pixel_rect()`. Do not add `kernel/gui/pixel.c` in this task.

```c
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
```

- [ ] **Step 5: Expose the framebuffer primitive prototypes**

Modify `kernel/gui/framebuffer.h` by adding `#include "pixel.h"` after the existing includes and adding these prototypes after `framebuffer_draw_cursor()`:

```c
#include "pixel.h"

void framebuffer_draw_rect_outline(const framebuffer_info_t *fb,
                                   int x, int y, int w, int h,
                                   uint32_t color);
void framebuffer_draw_text_clipped(const framebuffer_info_t *fb,
                                   const gui_pixel_rect_t *clip,
                                   int x, int y,
                                   const char *text,
                                   uint32_t fg,
                                   uint32_t bg);
void framebuffer_draw_scrollbar(const framebuffer_info_t *fb,
                                int x, int y, int w, int h,
                                int total_rows,
                                int visible_rows,
                                int view_top,
                                uint32_t track,
                                uint32_t thumb);
```

- [ ] **Step 6: Implement the framebuffer primitives**

Add these functions to `kernel/gui/framebuffer.c`:

```c
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
    int col = 0;

    if (!fb || !clip || !text)
        return;
    while (text[col]) {
        framebuffer_draw_glyph_clipped(fb, clip,
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
```

- [ ] **Step 7: Run tests and commit**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds.

Commit:

```bash
git add kernel/gui/pixel.h kernel/gui/framebuffer.h kernel/gui/framebuffer.c kernel/test/test_desktop.c
git commit -m "feat: add framebuffer pixel primitives"
```

## Task 2: Terminal Surface Model

**Files:**
- Create: `kernel/gui/terminal.h`
- Create: `kernel/gui/terminal.c`
- Modify: `Makefile`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing terminal model tests**

Add these tests before the desktop integration tests in `kernel/test/test_desktop.c`:

```c
#include "terminal.h"

static gui_cell_t terminal_cells[16 * 4];
static gui_cell_t terminal_history[16 * 8];

static void test_terminal_write_wraps_and_retains_history(ktest_case_t *tc)
{
    gui_terminal_t term;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             4, 2, 4, 0x0f),
                    1);

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, "abcdX", 5), 5);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).ch, 'a');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 3, 0).ch, 'd');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 1).ch, 'X');

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, "\nzzzz\nq", 7), 7);
    KTEST_EXPECT_GE(tc, gui_terminal_history_count(&term), 1);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 1).ch, 'q');
}

static void test_terminal_ansi_color_does_not_emit_escape_bytes(ktest_case_t *tc)
{
    gui_terminal_t term;
    static const char text[] = "\x1b[32mG\x1b[0mW";

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 2, 4, 0x0f),
                    1);

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, text, sizeof(text) - 1),
                    sizeof(text) - 1);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).ch, 'G');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).attr, 0x0a);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 1, 0).ch, 'W');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 1, 0).attr, 0x0f);
}

static void test_terminal_clear_discards_history_and_resets_cursor(ktest_case_t *tc)
{
    gui_terminal_t term;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             4, 2, 4, 0x0f),
                    1);
    KTEST_ASSERT_EQ(tc, gui_terminal_write(&term, "abcd\nefgh\nijkl", 14), 14);

    gui_terminal_clear(&term);

    KTEST_EXPECT_EQ(tc, gui_terminal_history_count(&term), 0);
    KTEST_EXPECT_EQ(tc, gui_terminal_cursor_x(&term), 0);
    KTEST_EXPECT_EQ(tc, gui_terminal_cursor_y(&term), 0);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).ch, ' ');
}
```

Add the cases to `desktop_cases[]` before the desktop render cases:

```c
KTEST_CASE(test_terminal_write_wraps_and_retains_history),
KTEST_CASE(test_terminal_ansi_color_does_not_emit_escape_bytes),
KTEST_CASE(test_terminal_clear_discards_history_and_resets_cursor),
```

- [ ] **Step 2: Run the failing tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails because `terminal.h` and `gui_terminal_*` symbols do not exist.

- [ ] **Step 3: Add the terminal API**

Create `kernel/gui/terminal.h`:

```c
#ifndef GUI_TERMINAL_H
#define GUI_TERMINAL_H

#include "display.h"
#include "pixel.h"
#include <stdint.h>

typedef struct {
    gui_cell_t *live;
    gui_cell_t *history;
    int cols;
    int rows;
    int history_rows;
    int history_head;
    int history_count;
    int cursor_x;
    int cursor_y;
    int wrap_pending;
    int ansi_state;
    int ansi_val;
    uint8_t attr;
    uint8_t default_attr;
    int view_top;
    int live_view;
    gui_pixel_rect_t pixel_rect;
    int padding_x;
    int padding_y;
} gui_terminal_t;

int gui_terminal_init_static(gui_terminal_t *term,
                             gui_cell_t *live,
                             gui_cell_t *history,
                             int cols,
                             int rows,
                             int history_rows,
                             uint8_t default_attr);
int gui_terminal_init_alloc(gui_terminal_t *term,
                            int cols,
                            int rows,
                            int history_rows,
                            uint8_t default_attr);
void gui_terminal_clear(gui_terminal_t *term);
int gui_terminal_write(gui_terminal_t *term, const char *buf, uint32_t len);
void gui_terminal_scroll_view(gui_terminal_t *term, int rows);
void gui_terminal_snap_live(gui_terminal_t *term);
gui_cell_t gui_terminal_cell_at(const gui_terminal_t *term, int x, int y);
int gui_terminal_history_count(const gui_terminal_t *term);
int gui_terminal_cursor_x(const gui_terminal_t *term);
int gui_terminal_cursor_y(const gui_terminal_t *term);
int gui_terminal_visible_view_top(const gui_terminal_t *term);
int gui_terminal_total_rows(const gui_terminal_t *term);
void gui_terminal_set_pixel_rect(gui_terminal_t *term,
                                 gui_pixel_rect_t rect,
                                 int padding_x,
                                 int padding_y);
void gui_terminal_render(const gui_terminal_t *term,
                         const gui_pixel_surface_t *surface,
                         const gui_pixel_theme_t *theme,
                         int draw_cursor);

#endif /* GUI_TERMINAL_H */
```

- [ ] **Step 4: Add the terminal object to the build**

Modify `Makefile` by adding `kernel/gui/terminal.o` after `kernel/gui/desktop.o` in `KOBJS`:

```make
        kernel/gui/display.o kernel/gui/framebuffer.o kernel/gui/font8x16.o kernel/gui/desktop.o kernel/gui/terminal.o \
```

- [ ] **Step 5: Implement terminal state and byte handling**

Create `kernel/gui/terminal.c`. Start with the existing shell-cell behavior from `desktop.c`, but keep it owned by `gui_terminal_t`. Use this exact behavior table:

```text
ESC [ N m changes attr for N = 0, 31, 32, 33, 36
\r sets cursor_x to 0
\n moves to the next row and scrolls if needed
\b clears wrap_pending or moves cursor_x left by one
\t writes spaces up to the next four-column boundary
ordinary bytes write one cell and set wrap_pending at the final column
```

Implement these helpers in `terminal.c` with the names shown so later tasks can call them:

```c
static void terminal_clear_line(gui_terminal_t *term, int row);
static void terminal_push_history(gui_terminal_t *term, const gui_cell_t *row);
static void terminal_scroll_up(gui_terminal_t *term);
static void terminal_apply_ansi(gui_terminal_t *term, int code);
static void terminal_apply_char(gui_terminal_t *term, char c);
```

The scroll implementation must copy live rows up one row, clear the last row, decrement `cursor_y` when needed, and append the evicted top row into the history ring:

```c
static void terminal_scroll_up(gui_terminal_t *term)
{
    int bytes;

    if (!term || !term->live || term->cols <= 0 || term->rows <= 0)
        return;
    terminal_push_history(term, term->live);
    bytes = term->cols * (int)sizeof(gui_cell_t);
    for (int row = 1; row < term->rows; row++) {
        k_memmove(&term->live[(row - 1) * term->cols],
                  &term->live[row * term->cols],
                  (uint32_t)bytes);
    }
    terminal_clear_line(term, term->rows - 1);
    if (term->cursor_y > 0)
        term->cursor_y--;
}
```

Use `kmalloc` in `gui_terminal_init_alloc()` and return `0` if either allocation fails. If the second allocation fails, free the first allocation before returning `0`.

- [ ] **Step 6: Run tests and commit**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds and the KTEST kernel links.

Commit:

```bash
git add Makefile kernel/gui/terminal.h kernel/gui/terminal.c kernel/test/test_desktop.c
git commit -m "feat: add gui terminal surface model"
```

## Task 3: Terminal Pixel Rendering

**Files:**
- Modify: `kernel/gui/terminal.c`
- Modify: `kernel/gui/terminal.h`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing render tests**

Add these tests:

```c
static uint32_t terminal_pixels[128 * 96];

static void terminal_test_fb(framebuffer_info_t *fb)
{
    k_memset(terminal_pixels, 0, sizeof(terminal_pixels));
    k_memset(fb, 0, sizeof(*fb));
    fb->address = (uintptr_t)terminal_pixels;
    fb->pitch = 128u * sizeof(uint32_t);
    fb->width = 128u;
    fb->height = 96u;
    fb->red_pos = 16u;
    fb->red_size = 8u;
    fb->green_pos = 8u;
    fb->green_size = 8u;
    fb->blue_pos = 0u;
    fb->blue_size = 8u;
}

static void test_terminal_render_uses_pixel_padding(ktest_case_t *tc)
{
    gui_terminal_t term;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;

    terminal_test_fb(&fb);
    k_memset(&theme, 0, sizeof(theme));
    theme.terminal_bg = 0x00101010u;
    theme.terminal_fg = 0x00FFFFFFu;
    theme.terminal_cursor = 0x00FFCC44u;
    surface.fb = &fb;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = 128;
    surface.clip.h = 96;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 3, 4, 0x0f),
                    1);
    gui_terminal_set_pixel_rect(&term,
                                (gui_pixel_rect_t){ 10, 12, 80, 60 },
                                6, 5);
    KTEST_ASSERT_EQ(tc, gui_terminal_write(&term, "A", 1), 1);

    gui_terminal_render(&term, &surface, &theme, 1);

    KTEST_EXPECT_EQ(tc, terminal_pixels[12 * 128 + 10], 0x00101010u);
    KTEST_EXPECT_NE(tc, terminal_pixels[17 * 128 + 16], 0u);
}

static void test_terminal_render_draws_underline_cursor(ktest_case_t *tc)
{
    gui_terminal_t term;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;
    int cursor_y;

    terminal_test_fb(&fb);
    k_memset(&theme, 0, sizeof(theme));
    theme.terminal_bg = 0x00000000u;
    theme.terminal_fg = 0x00FFFFFFu;
    theme.terminal_cursor = 0x00FFCC44u;
    surface.fb = &fb;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = 128;
    surface.clip.h = 96;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 3, 4, 0x0f),
                    1);
    gui_terminal_set_pixel_rect(&term,
                                (gui_pixel_rect_t){ 0, 0, 80, 60 },
                                4, 4);

    gui_terminal_render(&term, &surface, &theme, 1);

    cursor_y = 4 + (int)GUI_FONT_H - 2;
    KTEST_EXPECT_EQ(tc, terminal_pixels[cursor_y * 128 + 4],
                    0x00FFCC44u);
}
```

Add cases:

```c
KTEST_CASE(test_terminal_render_uses_pixel_padding),
KTEST_CASE(test_terminal_render_draws_underline_cursor),
```

- [ ] **Step 2: Run the failing tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: either compile fails because render functions are incomplete, or tests fail because rendering does not honor padding/cursor pixels yet.

- [ ] **Step 3: Implement terminal pixel geometry**

In `gui_terminal_set_pixel_rect()`, store the rectangle and padding, then recompute usable terminal columns and rows only when the caller has not already initialized live buffers. For this milestone, desktop integration will initialize the terminal after computing geometry, so the setter only stores geometry:

```c
void gui_terminal_set_pixel_rect(gui_terminal_t *term,
                                 gui_pixel_rect_t rect,
                                 int padding_x,
                                 int padding_y)
{
    if (!term)
        return;
    term->pixel_rect = rect;
    term->padding_x = padding_x < 0 ? 0 : padding_x;
    term->padding_y = padding_y < 0 ? 0 : padding_y;
}
```

- [ ] **Step 4: Implement terminal rendering**

In `gui_terminal_render()`, draw the terminal background, then draw visible rows. Use this mapping:

```text
content_x = pixel_rect.x + padding_x
content_y = pixel_rect.y + padding_y
cell_px_x = content_x + col * GUI_FONT_W
cell_px_y = content_y + row * GUI_FONT_H
```

For live view, render rows from `term->live`. For scrollback view, compose from history plus live rows using `view_top`. The first visible global row is `view_top`, where global rows are `history_count + rows` tall. When `live_view` is true, `view_top = max(0, total_rows - rows)`.

Render the underline cursor only when `draw_cursor` is true and `term->live_view` is true:

```c
framebuffer_fill_rect(surface->fb,
                      content_x + term->cursor_x * (int)GUI_FONT_W,
                      content_y + term->cursor_y * (int)GUI_FONT_H +
                          (int)GUI_FONT_H - 2,
                      (int)GUI_FONT_W,
                      2,
                      theme->terminal_cursor);
```

- [ ] **Step 5: Run tests and commit**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds.

Commit:

```bash
git add kernel/gui/terminal.h kernel/gui/terminal.c kernel/test/test_desktop.c
git commit -m "feat: render terminal as pixels"
```

## Task 4: Desktop Framebuffer Pixel Path

**Files:**
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/gui/desktop.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing desktop integration tests**

Add these tests:

```c
static void test_framebuffer_desktop_renders_pixel_terminal_background(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    int terminal_px;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    terminal_px = (desktop.shell_pixel_rect.y + 6) * 480 +
                  (desktop.shell_pixel_rect.x + 6);
    KTEST_EXPECT_NE(tc, pointer_motion_pixels[terminal_px], 0u);
}

static void test_framebuffer_desktop_shell_output_renders_inside_terminal(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    int text_px;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "A", 1), 1);

    text_px = (desktop.shell_pixel_rect.y + 10) * 480 +
              (desktop.shell_pixel_rect.x + 10);
    KTEST_EXPECT_NE(tc, pointer_motion_pixels[text_px], 0u);
}
```

Add cases after the existing framebuffer shell write test:

```c
KTEST_CASE(test_framebuffer_desktop_renders_pixel_terminal_background),
KTEST_CASE(test_framebuffer_desktop_shell_output_renders_inside_terminal),
```

- [ ] **Step 2: Run failing tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails because `desktop.shell_pixel_rect` does not exist, or tests fail because desktop framebuffer mode still relies on cell presentation.

- [ ] **Step 3: Update desktop state**

Modify `desktop_state_t` in `kernel/gui/desktop.h`:

```c
#include "terminal.h"

gui_pixel_rect_t taskbar_pixel_rect;
gui_pixel_rect_t launcher_pixel_rect;
gui_pixel_rect_t window_pixel_rect;
gui_pixel_rect_t shell_pixel_rect;
gui_terminal_t shell_terminal;
```

Keep `shell_cells`, `shell_cells_w`, `shell_cells_h`, `shell_cursor_x`, and related fields during the transition if VGA tests still depend on them. Once terminal integration passes for both framebuffer and VGA fallback, remove the duplicate shell-cell fields and update tests to read terminal state through `gui_terminal_*`.

- [ ] **Step 4: Add pixel layout calculation**

In `desktop_layout()`, after cell rectangles are computed, derive pixel rectangles:

```c
desktop->taskbar_pixel_rect.x = 0;
desktop->taskbar_pixel_rect.y =
    (desktop->display->rows - 1) * (int)GUI_FONT_H;
desktop->taskbar_pixel_rect.w = desktop->display->cols * (int)GUI_FONT_W;
desktop->taskbar_pixel_rect.h = (int)GUI_FONT_H;

desktop->window_pixel_rect.x = desktop->shell_rect.x * (int)GUI_FONT_W;
desktop->window_pixel_rect.y = desktop->shell_rect.y * (int)GUI_FONT_H;
desktop->window_pixel_rect.w = desktop->shell_rect.w * (int)GUI_FONT_W;
desktop->window_pixel_rect.h = desktop->shell_rect.h * (int)GUI_FONT_H;

desktop->shell_pixel_rect.x = desktop->window_pixel_rect.x + 8;
desktop->shell_pixel_rect.y = desktop->window_pixel_rect.y + 24;
desktop->shell_pixel_rect.w = desktop->window_pixel_rect.w - 16;
desktop->shell_pixel_rect.h = desktop->window_pixel_rect.h - 32;
```

Clamp `shell_pixel_rect.w` and `shell_pixel_rect.h` to non-negative values before using them.

- [ ] **Step 5: Initialize terminal geometry in desktop init**

In `desktop_init()`, when framebuffer mode is not known yet, initialize the terminal from the cell shell content so VGA fallback stays valid:

```c
desktop->shell_cells_w = desktop->shell_content.w;
desktop->shell_cells_h = desktop->shell_content.h;
```

Then initialize `shell_terminal` with the same dimensions:

```c
if (!gui_terminal_init_alloc(&desktop->shell_terminal,
                             desktop->shell_content.w,
                             desktop->shell_content.h,
                             500,
                             display->default_attr)) {
    desktop->active = 0;
    desktop->desktop_enabled = 0;
    return;
}
gui_terminal_set_pixel_rect(&desktop->shell_terminal,
                            desktop->shell_pixel_rect,
                            8,
                            6);
```

- [ ] **Step 6: Render framebuffer desktop with pixel primitives**

Add a framebuffer-only render helper in `desktop.c`:

```c
static void desktop_render_framebuffer(desktop_state_t *desktop)
{
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;

    desktop_pixel_theme(desktop->framebuffer, &theme);
    surface.fb = desktop->framebuffer;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = (int)desktop->framebuffer->width;
    surface.clip.h = (int)desktop->framebuffer->height;

    framebuffer_fill_rect(desktop->framebuffer, 0, 0,
                          (int)desktop->framebuffer->width,
                          (int)desktop->framebuffer->height,
                          theme.desktop_bg);
    framebuffer_fill_rect(desktop->framebuffer,
                          desktop->taskbar_pixel_rect.x,
                          desktop->taskbar_pixel_rect.y,
                          desktop->taskbar_pixel_rect.w,
                          desktop->taskbar_pixel_rect.h,
                          theme.taskbar_bg);
    framebuffer_draw_text_clipped(desktop->framebuffer,
                                  &surface.clip,
                                  16,
                                  desktop->taskbar_pixel_rect.y,
                                  "Menu",
                                  theme.taskbar_fg,
                                  theme.taskbar_bg);
    if (desktop->shell_window_open) {
        framebuffer_fill_rect(desktop->framebuffer,
                              desktop->window_pixel_rect.x,
                              desktop->window_pixel_rect.y,
                              desktop->window_pixel_rect.w,
                              desktop->window_pixel_rect.h,
                              theme.window_bg);
        framebuffer_fill_rect(desktop->framebuffer,
                              desktop->window_pixel_rect.x,
                              desktop->window_pixel_rect.y,
                              desktop->window_pixel_rect.w,
                              20,
                              theme.title_bg);
        framebuffer_draw_rect_outline(desktop->framebuffer,
                                      desktop->window_pixel_rect.x,
                                      desktop->window_pixel_rect.y,
                                      desktop->window_pixel_rect.w,
                                      desktop->window_pixel_rect.h,
                                      theme.window_border);
        framebuffer_draw_text_clipped(desktop->framebuffer,
                                      &surface.clip,
                                      desktop->window_pixel_rect.x + 8,
                                      desktop->window_pixel_rect.y + 2,
                                      "Shell",
                                      theme.title_fg,
                                      theme.title_bg);
        gui_terminal_render(&desktop->shell_terminal,
                            &surface,
                            &theme,
                            1);
    }
    desktop_draw_framebuffer_pointer(desktop);
}
```

Update `desktop_render()` so framebuffer mode calls `desktop_render_framebuffer(desktop)` instead of `gui_display_present_to_framebuffer()`.

- [ ] **Step 7: Route console output through the terminal**

In `desktop_write_console_output()`, call:

```c
written = gui_terminal_write(&desktop->shell_terminal, buf, len);
if (desktop->framebuffer_enabled && desktop->framebuffer)
    desktop_render_framebuffer(desktop);
else
    desktop_render(desktop);
return written;
```

For VGA fallback, keep copying terminal live cells into `gui_display_t` through a helper:

```c
static void desktop_terminal_redraw_to_cells(desktop_state_t *desktop)
{
    for (int row = 0; row < desktop->shell_content.h; row++) {
        for (int col = 0; col < desktop->shell_content.w; col++) {
            gui_cell_t cell = gui_terminal_cell_at(&desktop->shell_terminal,
                                                   col, row);
            int dx = desktop->shell_content.x + col;
            int dy = desktop->shell_content.y + row;
            if (dx >= 0 && dy >= 0 &&
                dx < desktop->display->cols &&
                dy < desktop->display->rows)
                desktop->display->cells[dy * desktop->display->cols + dx] =
                    cell;
        }
    }
}
```

- [ ] **Step 8: Run tests and commit**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds.

Commit:

```bash
git add kernel/gui/desktop.h kernel/gui/desktop.c kernel/test/test_desktop.c
git commit -m "feat: render desktop through pixel terminal"
```

## Task 5: Desktop Scrollback Syscalls

**Files:**
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/proc/syscall.c`
- Test: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write failing syscall scrollback tests**

Add:

```c
static void test_desktop_scroll_syscall_moves_terminal_view(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);

    KTEST_ASSERT_EQ(tc,
                    desktop_write_console_output(&desktop,
                                                 "aaaa\nbbbb\ncccc\ndddd\n",
                                                 20),
                    20);
    KTEST_EXPECT_EQ(tc,
                    desktop_scroll_console(&desktop, -1),
                    1);
    KTEST_EXPECT_FALSE(tc, desktop.shell_terminal.live_view);

    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_SCROLL_DOWN, 1, 0, 0, 0, 0), 0);
    KTEST_EXPECT_TRUE(tc, desktop.shell_terminal.live_view);
}

static void test_desktop_new_output_snaps_scrollback_to_live(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_ASSERT_EQ(tc,
                    desktop_write_console_output(&desktop,
                                                 "aaaa\nbbbb\ncccc\ndddd\n",
                                                 20),
                    20);
    KTEST_ASSERT_EQ(tc, desktop_scroll_console(&desktop, -1), 1);
    KTEST_ASSERT_TRUE(tc, !desktop.shell_terminal.live_view);

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "x", 1), 1);
    KTEST_EXPECT_TRUE(tc, desktop.shell_terminal.live_view);
}
```

Add cases:

```c
KTEST_CASE(test_desktop_scroll_syscall_moves_terminal_view),
KTEST_CASE(test_desktop_new_output_snaps_scrollback_to_live),
```

- [ ] **Step 2: Run failing tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile fails because `desktop_scroll_console()` does not exist, or tests fail because syscalls still return without touching desktop scrollback.

- [ ] **Step 3: Add desktop scroll API**

Add to `kernel/gui/desktop.h`:

```c
int desktop_scroll_console(desktop_state_t *desktop, int rows);
```

Add to `desktop.c`:

```c
int desktop_scroll_console(desktop_state_t *desktop, int rows)
{
    if (!desktop || !desktop->active || !desktop->desktop_enabled)
        return 0;
    if (!desktop->shell_window_open)
        return 0;
    gui_terminal_scroll_view(&desktop->shell_terminal, rows);
    if (desktop->framebuffer_enabled && desktop->framebuffer)
        desktop_render_framebuffer(desktop);
    else
        desktop_render(desktop);
    return 1;
}
```

- [ ] **Step 4: Route syscalls**

Modify `kernel/proc/syscall.c`:

```c
case SYS_SCROLL_UP:
    if (desktop_is_active() &&
        desktop_scroll_console(desktop_global(), -(int)ebx))
        return 0;
    scroll_up((int)ebx);
    return 0;

case SYS_SCROLL_DOWN:
    if (desktop_is_active() &&
        desktop_scroll_console(desktop_global(), (int)ebx))
        return 0;
    scroll_down((int)ebx);
    return 0;
```

- [ ] **Step 5: Run tests and commit**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds.

Commit:

```bash
git add kernel/gui/desktop.h kernel/gui/desktop.c kernel/proc/syscall.c kernel/test/test_desktop.c
git commit -m "feat: route desktop scrollback syscalls"
```

## Task 6: Documentation And Final Verification

**Files:**
- Modify: `docs/ch03-kernel-entry.md`
- Modify: `docs/ch22-shell.md`

- [ ] **Step 1: Update Chapter 3 framebuffer desktop prose**

In `docs/ch03-kernel-entry.md`, revise the framebuffer paragraph that currently says the graphical desktop still uses a text grid internally. Use this replacement paragraph:

```markdown
The graphical desktop now splits its presentation path by display backend. VGA fallback still presents a character grid directly to the text buffer, but framebuffer mode treats the shell window as a pixel terminal surface. The shell still writes text and ANSI colour changes through the same console path, but the desktop owns the pixel rectangle around that text: padding, title bar, border, underline cursor, scrollbar, and pointer are all drawn as framebuffer pixels. The terminal computes how many `8x16` glyph cells fit inside its padded pixel rectangle, so the shell remains text-oriented while the desktop around it is no longer just a full-screen cell buffer painted into pixels.
```

- [ ] **Step 2: Update Chapter 22 scrollback behavior**

In `docs/ch22-shell.md`, revise the paragraph that says Page Up and Page Down are ignored while the framebuffer desktop is active. Use this replacement sentence:

```markdown
Page Up and Page Down invoke the same scrollback syscalls in both display modes: VGA fallback scrolls the legacy console history, while framebuffer desktop mode scrolls the shell terminal surface and updates its pixel scrollbar.
```

- [ ] **Step 3: Run build verification**

Run:

```bash
make kernel disk
```

Expected: build succeeds, producing `kernel.elf`, `os.iso`, and `disk.img`. Existing non-fatal linker warnings about RWX load segments may still appear.

- [ ] **Step 4: Run bounded test verification**

Run:

```bash
make KTEST=1 kernel
```

Expected: build succeeds and links the KTEST-enabled kernel. If a bounded QEMU smoke target is available at this point, run it and inspect `debugcon.log` for the desktop suite result. Do not run `make test` as an unattended final check because QEMU intentionally stays open.

- [ ] **Step 5: Commit documentation and any final fixes**

Commit:

```bash
git add docs/ch03-kernel-entry.md docs/ch22-shell.md
git commit -m "docs: describe pixel terminal desktop"
```

- [ ] **Step 6: Push the completed branch**

After all implementation commits are present and verification evidence is fresh, push the branch for cross-machine review:

```bash
git push -u origin feature/new-work
```

Expected: the remote branch `origin/feature/new-work` exists and tracks the local branch.

## Self-Review Checklist

- The spec requirement for a pixel surface boundary is covered by Task 1 and Task 4.
- The spec requirement for a terminal surface module is covered by Task 2 and Task 3.
- The spec requirement for pixel chrome and terminal rendering is covered by Task 4.
- The spec requirement for keyboard scrollback and a visible scrollbar is covered by Task 3 and Task 5.
- The spec requirement for keeping VGA fallback is covered by Task 4.
- The spec requirement for syscall scroll integration is covered by Task 5.
- The spec requirement for documentation updates is covered by Task 6.
- The user request to push the completed feature branch is covered by Task 6.

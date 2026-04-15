# GUI Desktop Shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot DrunixOS into a text-mode desktop shell with a taskbar, launcher, auto-opened shell window, basic keyboard and mouse interaction, and a safe fallback to the legacy full-screen console.

**Architecture:** Add a small kernel GUI stack in front of the existing VGA console: a VGA-backed display abstraction, a desktop shell with fixed layout and launcher/focus state, and output/input routing that forwards the boot shell through a bounded surface instead of the whole screen. Keep the first release text-mode only, but isolate the desktop logic behind backend-neutral display and input APIs so a future pixel renderer can replace the VGA backend without rewriting desktop behavior.

**Tech Stack:** freestanding C, x86 IRQ handlers, VGA text memory, existing TTY/process/syscall stack, KTEST in-kernel unit tests, QEMU, GNU make.

---

## File Structure

- Create: `kernel/gui/display.h`
  Responsibility: cell/rect/display primitives, clipping helpers, dirty-region metadata, and VGA-independent drawing APIs.
- Create: `kernel/gui/display.c`
  Responsibility: in-memory text-cell backend, bounded fill/text/border drawing, VGA present helpers, and cursor visibility control.
- Create: `kernel/gui/desktop.h`
  Responsibility: desktop state model, focus/launcher enums, keyboard and pointer event APIs, shell surface attachment, and activation helpers.
- Create: `kernel/gui/desktop.c`
  Responsibility: fixed-layout desktop renderer, launcher/taskbar logic, shell window management, shell stdout routing, and desktop-active/fallback flags.
- Create: `kernel/drivers/mouse.h`
  Responsibility: PS/2 mouse init API and packet-to-pointer event definitions.
- Create: `kernel/drivers/mouse.c`
  Responsibility: IRQ12 registration, PS/2 controller bring-up, 3-byte packet decoding, pointer clamping, and desktop click delivery.
- Create: `kernel/test/test_desktop.c`
  Responsibility: unit coverage for display clipping, desktop layout, keyboard routing, mouse decode/interaction, and shell output routing.
- Modify: `Makefile`
  Responsibility: add `kernel/gui` include path, desktop GUI objects, mouse object, and the new desktop test object to kernel/test builds.
- Modify: `kernel/kernel.c`
  Responsibility: initialize the desktop subsystem during boot, initialize the mouse driver, attach the boot shell to the desktop, and preserve a clean legacy fallback path.
- Modify: `kernel/drivers/keyboard.c`
  Responsibility: route decoded key events through the desktop shell before falling back to the TTY.
- Modify: `kernel/lib/klog.c`
  Responsibility: stop mirroring kernel logs onto the on-screen console after desktop takeover while keeping debugcon and the retained klog ring active.
- Modify: `kernel/proc/syscall.c`
  Responsibility: route `FD_TYPE_STDOUT` bytes into the hosted shell surface when the desktop is active, while preserving the legacy VGA path when it is not.
- Modify: `kernel/test/ktest.h`
  Responsibility: declare the desktop test suite registration hook.
- Modify: `kernel/test/ktest.c`
  Responsibility: register the desktop suite in `ktest_run_all()`.

## Task 1: Add The GUI Display Backend And Test Harness

**Files:**
- Create: `kernel/gui/display.h`
- Create: `kernel/gui/display.c`
- Create: `kernel/test/test_desktop.c`
- Modify: `Makefile`
- Modify: `kernel/test/ktest.h`
- Modify: `kernel/test/ktest.c`

- [ ] **Step 1: Write the failing display tests**

```c
/* kernel/test/test_desktop.c */
#include "ktest.h"
#include "display.h"

static void test_gui_display_fill_rect_clips_to_bounds(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    gui_rect_t dirty;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    dirty = gui_display_fill_rect(&display, -2, 0, 4, 1, ' ', 0x1f);

    KTEST_EXPECT_EQ(tc, dirty.x, 0);
    KTEST_EXPECT_EQ(tc, dirty.y, 0);
    KTEST_EXPECT_EQ(tc, dirty.w, 2);
    KTEST_EXPECT_EQ(tc, dirty.h, 1);
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 0, 0).attr, 0x1f);
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 2, 0).attr, 0x0f);
}

static void test_gui_display_draw_text_stops_at_region_edge(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    gui_display_draw_text(&display, 3, 2, 4, "desktop", 0x0e);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 3, 2).ch, 'd');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 6, 2).ch, 'k');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 7, 2).ch, ' ');
}

static ktest_case_t desktop_cases[] = {
    KTEST_CASE(test_gui_display_fill_rect_clips_to_bounds),
    KTEST_CASE(test_gui_display_draw_text_stops_at_region_edge),
};

ktest_suite_t *ktest_suite_desktop(void)
{
    static ktest_suite_t suite = KTEST_SUITE("desktop", desktop_cases);
    return &suite;
}
```

```c
/* kernel/test/ktest.h */
ktest_suite_t *ktest_suite_desktop(void);
```

```c
/* kernel/test/ktest.c */
    ktest_run_suite(ktest_suite_desktop());
```

```make
# Makefile
INC += -I kernel/gui
KTOBJS += kernel/test/test_desktop.o
KOBJS += kernel/gui/display.o
```

- [ ] **Step 2: Run the kernel build to verify it fails before the backend exists**

Run: `make KTEST=1 kernel`

Expected: FAIL with missing declarations or undefined references for `gui_display_init`, `gui_display_fill_rect`, `gui_display_draw_text`, and `gui_display_cell_at`.

- [ ] **Step 3: Write the minimal display backend**

```c
/* kernel/gui/display.h */
#ifndef GUI_DISPLAY_H
#define GUI_DISPLAY_H

#include <stdint.h>

typedef struct {
    int x;
    int y;
    int w;
    int h;
} gui_rect_t;

typedef struct {
    char ch;
    uint8_t attr;
} gui_cell_t;

typedef struct {
    gui_cell_t *cells;
    int cols;
    int rows;
    uint8_t default_attr;
    int cursor_x;
    int cursor_y;
    int cursor_visible;
} gui_display_t;

void gui_display_init(gui_display_t *display, gui_cell_t *cells,
                      int cols, int rows, uint8_t default_attr);
gui_rect_t gui_display_fill_rect(gui_display_t *display,
                                 int x, int y, int w, int h,
                                 char ch, uint8_t attr);
gui_rect_t gui_display_draw_text(gui_display_t *display,
                                 int x, int y, int max_w,
                                 const char *text, uint8_t attr);
gui_rect_t gui_display_draw_frame(gui_display_t *display,
                                  int x, int y, int w, int h,
                                  uint8_t attr);
gui_cell_t gui_display_cell_at(const gui_display_t *display, int x, int y);
void gui_display_set_cursor(gui_display_t *display, int x, int y, int visible);
void gui_display_present_to_vga(const gui_display_t *display, uintptr_t video_address);

#endif
```

```c
/* kernel/gui/display.c */
#include "display.h"
#include "kstring.h"

static gui_rect_t gui_clip_rect(const gui_display_t *display,
                                int x, int y, int w, int h)
{
    gui_rect_t out;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > display->cols) w = display->cols - x;
    if (y + h > display->rows) h = display->rows - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    out.x = x;
    out.y = y;
    out.w = w;
    out.h = h;
    return out;
}

void gui_display_init(gui_display_t *display, gui_cell_t *cells,
                      int cols, int rows, uint8_t default_attr)
{
    display->cells = cells;
    display->cols = cols;
    display->rows = rows;
    display->default_attr = default_attr;
    display->cursor_x = 0;
    display->cursor_y = 0;
    display->cursor_visible = 1;
    for (int i = 0; i < cols * rows; i++) {
        display->cells[i].ch = ' ';
        display->cells[i].attr = default_attr;
    }
}

gui_rect_t gui_display_fill_rect(gui_display_t *display,
                                 int x, int y, int w, int h,
                                 char ch, uint8_t attr)
{
    gui_rect_t dirty = gui_clip_rect(display, x, y, w, h);

    for (int row = dirty.y; row < dirty.y + dirty.h; row++) {
        for (int col = dirty.x; col < dirty.x + dirty.w; col++) {
            gui_cell_t *cell = &display->cells[row * display->cols + col];
            cell->ch = ch;
            cell->attr = attr;
        }
    }

    return dirty;
}

gui_rect_t gui_display_draw_text(gui_display_t *display,
                                 int x, int y, int max_w,
                                 const char *text, uint8_t attr)
{
    int written = 0;

    while (text[written] && written < max_w && x + written < display->cols) {
        if (x + written >= 0 && y >= 0 && y < display->rows) {
            gui_cell_t *cell = &display->cells[y * display->cols + x + written];
            cell->ch = text[written];
            cell->attr = attr;
        }
        written++;
    }

    return gui_clip_rect(display, x, y, written, 1);
}

gui_rect_t gui_display_draw_frame(gui_display_t *display,
                                  int x, int y, int w, int h,
                                  uint8_t attr)
{
    gui_display_fill_rect(display, x, y, w, 1, '-', attr);
    gui_display_fill_rect(display, x, y + h - 1, w, 1, '-', attr);
    gui_display_fill_rect(display, x, y, 1, h, '|', attr);
    gui_display_fill_rect(display, x + w - 1, y, 1, h, '|', attr);
    return gui_clip_rect(display, x, y, w, h);
}

gui_cell_t gui_display_cell_at(const gui_display_t *display, int x, int y)
{
    gui_cell_t blank = { ' ', display->default_attr };

    if (x < 0 || y < 0 || x >= display->cols || y >= display->rows)
        return blank;
    return display->cells[y * display->cols + x];
}

void gui_display_set_cursor(gui_display_t *display, int x, int y, int visible)
{
    display->cursor_x = x;
    display->cursor_y = y;
    display->cursor_visible = visible;
}

void gui_display_present_to_vga(const gui_display_t *display, uintptr_t video_address)
{
    volatile unsigned char *vidmem = (volatile unsigned char *)video_address;

    for (int row = 0; row < display->rows; row++) {
        for (int col = 0; col < display->cols; col++) {
            const gui_cell_t *cell = &display->cells[row * display->cols + col];
            int off = 2 * (row * display->cols + col);
            vidmem[off] = (unsigned char)cell->ch;
            vidmem[off + 1] = cell->attr;
        }
    }
}
```

- [ ] **Step 4: Run the kernel build again to verify the display backend compiles**

Run: `make KTEST=1 kernel`

Expected: PASS, producing `kernel.elf` and `os.iso` with the new `desktop` KTEST suite linked in.

- [ ] **Step 5: Run the desktop test suite in QEMU and confirm there are no display-test failures**

Run: `make disk`

Expected: PASS, producing `disk.img`.

Run: `make test`

Expected: QEMU boots, runs KTEST, and remains open at the shell after the test suite completes.

Run: `pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"`

Expected: QEMU terminates cleanly.

Run: `rg "KTEST|desktop|FAIL" debugcon.log`

Expected: lines for the `desktop` suite and no `KTEST FAIL` entries for the new display tests.

- [ ] **Step 6: Commit the display backend groundwork**

```bash
git add Makefile \
        kernel/gui/display.h \
        kernel/gui/display.c \
        kernel/test/ktest.h \
        kernel/test/ktest.c \
        kernel/test/test_desktop.c
git commit -m "feat: add GUI display backend primitives"
```

## Task 2: Build The Desktop Layout Model And Renderer

**Files:**
- Create: `kernel/gui/desktop.h`
- Create: `kernel/gui/desktop.c`
- Modify: `Makefile`
- Modify: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write the failing desktop-layout tests**

```c
/* kernel/test/test_desktop.c */
#include "desktop.h"

static void test_desktop_boot_layout_opens_shell_window(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_TRUE(tc, desktop.active);
    KTEST_EXPECT_TRUE(tc, desktop.shell_window_open);
    KTEST_EXPECT_FALSE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_SHELL);
    KTEST_EXPECT_EQ(tc, desktop.taskbar.y, 24);
    KTEST_EXPECT_GE(tc, desktop.shell_rect.w, 48);
}

static void test_desktop_render_draws_taskbar_and_launcher_label(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 0, 24).ch, ' ');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 2, 24).ch, 'M');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 3, 24).ch, 'e');
}
```

- [ ] **Step 2: Run the kernel build to verify it fails before the desktop layer exists**

Run: `make KTEST=1 kernel`

Expected: FAIL with missing declarations or undefined references for `desktop_init`, `desktop_open_shell_window`, and `desktop_render`.

- [ ] **Step 3: Write the minimal desktop state model and renderer**

```c
/* kernel/gui/desktop.h */
#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include "display.h"
#include <stdint.h>

typedef enum {
    DESKTOP_FOCUS_SHELL = 0,
    DESKTOP_FOCUS_TASKBAR = 1,
    DESKTOP_FOCUS_LAUNCHER = 2,
} desktop_focus_t;

typedef struct {
    int x;
    int y;
    int dx;
    int dy;
    int left_down;
} desktop_pointer_event_t;

typedef struct {
    int active;
    int launcher_open;
    int shell_window_open;
    int desktop_enabled;
    desktop_focus_t focus;
    gui_rect_t taskbar;
    gui_rect_t launcher_rect;
    gui_rect_t shell_rect;
    gui_rect_t shell_content;
    gui_display_t *display;
    uint32_t shell_pid;
} desktop_state_t;

void desktop_init(desktop_state_t *desktop, gui_display_t *display);
void desktop_render(desktop_state_t *desktop);
void desktop_open_shell_window(desktop_state_t *desktop);

#endif
```

```c
/* kernel/gui/desktop.c */
#include "desktop.h"
#include "kstring.h"

static void desktop_layout(desktop_state_t *desktop)
{
    desktop->taskbar.x = 0;
    desktop->taskbar.y = desktop->display->rows - 1;
    desktop->taskbar.w = desktop->display->cols;
    desktop->taskbar.h = 1;

    desktop->launcher_rect.x = 1;
    desktop->launcher_rect.y = desktop->display->rows - 6;
    desktop->launcher_rect.w = 18;
    desktop->launcher_rect.h = 5;

    desktop->shell_rect.x = 6;
    desktop->shell_rect.y = 3;
    desktop->shell_rect.w = desktop->display->cols - 12;
    desktop->shell_rect.h = desktop->display->rows - 7;

    desktop->shell_content.x = desktop->shell_rect.x + 1;
    desktop->shell_content.y = desktop->shell_rect.y + 1;
    desktop->shell_content.w = desktop->shell_rect.w - 2;
    desktop->shell_content.h = desktop->shell_rect.h - 2;
}

void desktop_init(desktop_state_t *desktop, gui_display_t *display)
{
    k_memset(desktop, 0, sizeof(*desktop));
    desktop->display = display;
    desktop->active = 1;
    desktop->desktop_enabled = 1;
    desktop->focus = DESKTOP_FOCUS_TASKBAR;
    desktop_layout(desktop);
}

void desktop_open_shell_window(desktop_state_t *desktop)
{
    desktop->shell_window_open = 1;
    desktop->focus = DESKTOP_FOCUS_SHELL;
}

void desktop_render(desktop_state_t *desktop)
{
    gui_display_fill_rect(desktop->display, 0, 0,
                          desktop->display->cols, desktop->display->rows,
                          ' ', 0x1f);
    gui_display_fill_rect(desktop->display,
                          desktop->taskbar.x, desktop->taskbar.y,
                          desktop->taskbar.w, desktop->taskbar.h,
                          ' ', 0x70);
    gui_display_draw_text(desktop->display, 2, desktop->taskbar.y, 10,
                          "Menu", 0x70);

    if (desktop->launcher_open) {
        gui_display_draw_frame(desktop->display,
                               desktop->launcher_rect.x,
                               desktop->launcher_rect.y,
                               desktop->launcher_rect.w,
                               desktop->launcher_rect.h,
                               0x70);
        gui_display_draw_text(desktop->display,
                              desktop->launcher_rect.x + 2,
                              desktop->launcher_rect.y + 1,
                              desktop->launcher_rect.w - 4,
                              "Shell", 0x70);
    }

    if (desktop->shell_window_open) {
        gui_display_draw_frame(desktop->display,
                               desktop->shell_rect.x, desktop->shell_rect.y,
                               desktop->shell_rect.w, desktop->shell_rect.h,
                               0x1e);
        gui_display_draw_text(desktop->display,
                              desktop->shell_rect.x + 2,
                              desktop->shell_rect.y,
                              desktop->shell_rect.w - 4,
                              "Shell", 0x1e);
    }

    gui_display_present_to_vga(desktop->display, 0xB8000u);
}
```

```make
# Makefile
KOBJS += kernel/gui/desktop.o
```

- [ ] **Step 4: Run the kernel build again to verify the desktop layer compiles**

Run: `make KTEST=1 kernel`

Expected: PASS, producing a kernel image with the new desktop model linked in.

- [ ] **Step 5: Run KTEST and confirm the desktop layout suite passes**

Run: `make disk`

Expected: PASS.

Run: `make test`

Expected: QEMU boots and leaves KTEST results in `debugcon.log`.

Run: `pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"`

Expected: QEMU exits.

Run: `rg "desktop|KTEST FAIL" debugcon.log`

Expected: the `desktop` suite appears with no new failures.

- [ ] **Step 6: Commit the desktop layout and renderer**

```bash
git add Makefile \
        kernel/gui/desktop.h \
        kernel/gui/desktop.c \
        kernel/test/test_desktop.c
git commit -m "feat: add desktop shell layout renderer"
```

## Task 3: Route Keyboard Input Through The Desktop Shell

**Files:**
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/drivers/keyboard.c`
- Modify: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write the failing keyboard-routing tests**

```c
/* kernel/test/test_desktop.c */
static void test_desktop_escape_opens_launcher_and_consumes_input(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 27), DESKTOP_KEY_CONSUMED);
    KTEST_EXPECT_TRUE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_LAUNCHER);
}

static void test_desktop_plain_text_forwards_to_shell_when_focused(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'), DESKTOP_KEY_FORWARD);
    KTEST_EXPECT_FALSE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_SHELL);
}
```

- [ ] **Step 2: Run the kernel build to verify it fails before the input API exists**

Run: `make KTEST=1 kernel`

Expected: FAIL with missing declarations or undefined references for `desktop_handle_key` and `DESKTOP_KEY_CONSUMED`.

- [ ] **Step 3: Implement desktop keyboard handling and keyboard-driver routing**

```c
/* kernel/gui/desktop.h */
typedef enum {
    DESKTOP_KEY_FORWARD = 0,
    DESKTOP_KEY_CONSUMED = 1,
} desktop_key_result_t;

desktop_key_result_t desktop_handle_key(desktop_state_t *desktop, char c);
int desktop_is_active(void);
desktop_state_t *desktop_global(void);
```

```c
/* kernel/gui/desktop.c */
static desktop_state_t g_desktop;

desktop_state_t *desktop_global(void)
{
    return &g_desktop;
}

int desktop_is_active(void)
{
    return g_desktop.active && g_desktop.desktop_enabled;
}

desktop_key_result_t desktop_handle_key(desktop_state_t *desktop, char c)
{
    if (!desktop || !desktop->active)
        return DESKTOP_KEY_FORWARD;

    if (c == 27) {
        desktop->launcher_open = !desktop->launcher_open;
        desktop->focus = desktop->launcher_open
            ? DESKTOP_FOCUS_LAUNCHER
            : DESKTOP_FOCUS_SHELL;
        desktop_render(desktop);
        return DESKTOP_KEY_CONSUMED;
    }

    if (desktop->launcher_open && c == '\n') {
        desktop->launcher_open = 0;
        desktop_open_shell_window(desktop);
        desktop_render(desktop);
        return DESKTOP_KEY_CONSUMED;
    }

    if (desktop->launcher_open && c == '\t') {
        desktop->focus = DESKTOP_FOCUS_TASKBAR;
        desktop_render(desktop);
        return DESKTOP_KEY_CONSUMED;
    }

    return (desktop->focus == DESKTOP_FOCUS_SHELL)
        ? DESKTOP_KEY_FORWARD
        : DESKTOP_KEY_CONSUMED;
}
```

```c
/* kernel/drivers/keyboard.c */
#include "desktop.h"

static void keyboard_deliver_char(char c)
{
    if (desktop_is_active() &&
        desktop_handle_key(desktop_global(), c) == DESKTOP_KEY_CONSUMED)
        return;
    tty_input_char(0, c);
}

/* Replace tty_input_char(0, ...) call sites with keyboard_deliver_char(...) */
```

- [ ] **Step 4: Run the kernel build again to verify the keyboard routing compiles**

Run: `make KTEST=1 kernel`

Expected: PASS.

- [ ] **Step 5: Run KTEST and verify launcher/focus keyboard behavior**

Run: `make disk`

Expected: PASS.

Run: `make test`

Expected: QEMU boots, KTEST runs, and the shell remains available afterward.

Run: `pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"`

Expected: QEMU exits.

Run: `rg "desktop|FAIL" debugcon.log`

Expected: the new keyboard-routing tests pass with no `KTEST FAIL` lines from the desktop suite.

- [ ] **Step 6: Commit the keyboard-routing layer**

```bash
git add kernel/gui/desktop.h \
        kernel/gui/desktop.c \
        kernel/drivers/keyboard.c \
        kernel/test/test_desktop.c
git commit -m "feat: route keyboard input through desktop shell"
```

## Task 4: Add PS/2 Mouse Support And Pointer Interaction

**Files:**
- Create: `kernel/drivers/mouse.h`
- Create: `kernel/drivers/mouse.c`
- Modify: `Makefile`
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/kernel.c`
- Modify: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write the failing mouse and pointer tests**

```c
/* kernel/test/test_desktop.c */
#include "mouse.h"

static void test_mouse_packet_decode_clamps_pointer_to_screen(ktest_case_t *tc)
{
    desktop_pointer_event_t ev;
    mouse_packet_t packet = { .buttons = 0x09, .dx = -120, .dy = 60 };

    KTEST_EXPECT_EQ(tc, mouse_decode_packet(&packet, &ev), 0);
    KTEST_EXPECT_TRUE(tc, ev.left_down);
    KTEST_EXPECT_EQ(tc, ev.dx, -120);
    KTEST_EXPECT_EQ(tc, ev.dy, 60);
}

static void test_desktop_pointer_click_focuses_shell_window(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    desktop_state_t desktop;
    desktop_pointer_event_t ev = { .x = 10, .y = 5, .left_down = 1 };

    gui_display_init(&display, cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop.focus = DESKTOP_FOCUS_TASKBAR;

    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_SHELL);
}
```

- [ ] **Step 2: Run the kernel build to verify it fails before the mouse driver exists**

Run: `make KTEST=1 kernel`

Expected: FAIL with missing declarations or undefined references for `mouse_decode_packet`, `desktop_handle_pointer`, and `mouse_init`.

- [ ] **Step 3: Implement the PS/2 mouse driver and desktop pointer handling**

```c
/* kernel/drivers/mouse.h */
#ifndef MOUSE_H
#define MOUSE_H

#include "desktop.h"
#include <stdint.h>

typedef struct {
    uint8_t buttons;
    int8_t dx;
    int8_t dy;
} mouse_packet_t;

int mouse_decode_packet(const mouse_packet_t *packet, desktop_pointer_event_t *ev);
int mouse_init(void);

#endif
```

```c
/* kernel/drivers/mouse.c */
#include "mouse.h"
#include "desktop.h"
#include "irq.h"

#define PS2_DATA_PORT 0x60
#define PS2_CMD_PORT  0x64

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);

static uint8_t g_packet_bytes[3];
static int g_packet_index;
static int g_pointer_x = 40;
static int g_pointer_y = 12;

int mouse_decode_packet(const mouse_packet_t *packet, desktop_pointer_event_t *ev)
{
    if (!packet || !ev)
        return -1;

    ev->dx = packet->dx;
    ev->dy = -(int)packet->dy;
    ev->left_down = (packet->buttons & 0x1u) != 0;
    return 0;
}

static void mouse_handler(void)
{
    mouse_packet_t packet;
    desktop_pointer_event_t ev;

    g_packet_bytes[g_packet_index++] = port_byte_in(PS2_DATA_PORT);
    if (g_packet_index < 3)
        return;
    g_packet_index = 0;

    packet.buttons = g_packet_bytes[0];
    packet.dx = (int8_t)g_packet_bytes[1];
    packet.dy = (int8_t)g_packet_bytes[2];
    if (mouse_decode_packet(&packet, &ev) != 0)
        return;

    g_pointer_x += ev.dx;
    g_pointer_y += ev.dy;
    if (g_pointer_x < 0) g_pointer_x = 0;
    if (g_pointer_y < 0) g_pointer_y = 0;
    if (g_pointer_x > 79) g_pointer_x = 79;
    if (g_pointer_y > 24) g_pointer_y = 24;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;

    if (desktop_is_active())
        desktop_handle_pointer(desktop_global(), &ev);
}

int mouse_init(void)
{
    irq_register(12, mouse_handler);
    port_byte_out(PS2_CMD_PORT, 0xA8);
    port_byte_out(PS2_CMD_PORT, 0xD4);
    port_byte_out(PS2_DATA_PORT, 0xF4);
    return 0;
}
```

```c
/* kernel/gui/desktop.h */
void desktop_handle_pointer(desktop_state_t *desktop,
                            const desktop_pointer_event_t *ev);
```

```c
/* kernel/gui/desktop.c */
void desktop_handle_pointer(desktop_state_t *desktop,
                            const desktop_pointer_event_t *ev)
{
    if (!desktop || !ev)
        return;

    if (ev->left_down &&
        ev->x >= desktop->shell_rect.x &&
        ev->x < desktop->shell_rect.x + desktop->shell_rect.w &&
        ev->y >= desktop->shell_rect.y &&
        ev->y < desktop->shell_rect.y + desktop->shell_rect.h) {
        desktop->focus = DESKTOP_FOCUS_SHELL;
    }

    if (ev->left_down &&
        ev->x >= 1 && ev->x < 6 &&
        ev->y == desktop->taskbar.y) {
        desktop->launcher_open = !desktop->launcher_open;
        desktop->focus = desktop->launcher_open
            ? DESKTOP_FOCUS_LAUNCHER
            : DESKTOP_FOCUS_SHELL;
    }

    desktop_render(desktop);
}
```

```c
/* kernel/kernel.c */
extern int mouse_init(void);
...
    if (mouse_init() == 0)
        klog("IRQ", "mouse handler registered");
```

```make
# Makefile
KOBJS += kernel/drivers/mouse.o
```

- [ ] **Step 4: Run the kernel build again to verify mouse support compiles**

Run: `make KTEST=1 kernel`

Expected: PASS.

- [ ] **Step 5: Run KTEST and verify mouse decoding and pointer focus behavior**

Run: `make disk`

Expected: PASS.

Run: `make test`

Expected: PASS through KTEST, then interactive shell.

Run: `pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"`

Expected: QEMU exits.

Run: `rg "desktop|mouse|FAIL" debugcon.log`

Expected: the desktop suite passes without any new `KTEST FAIL` output.

- [ ] **Step 6: Commit mouse support**

```bash
git add Makefile \
        kernel/drivers/mouse.h \
        kernel/drivers/mouse.c \
        kernel/gui/desktop.h \
        kernel/gui/desktop.c \
        kernel/kernel.c \
        kernel/test/test_desktop.c
git commit -m "feat: add mouse input for desktop shell"
```

## Task 5: Route Boot-Shell Output Into The Desktop And Wire Fallback Boot

**Files:**
- Modify: `kernel/gui/desktop.h`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/lib/klog.c`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/kernel.c`
- Modify: `kernel/test/test_desktop.c`

- [ ] **Step 1: Write the failing shell-output and desktop-activation tests**

```c
/* kernel/test/test_desktop.c */
static void test_desktop_write_process_output_targets_shell_surface(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_pid(&desktop, 7);

    KTEST_EXPECT_EQ(tc,
                    desktop_write_process_output(&desktop, 7, "help", 4),
                    4);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    'h');
}

static void test_desktop_non_shell_output_is_rejected_for_legacy_fallback(ktest_case_t *tc)
{
    gui_cell_t cells[80 * 25];
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_pid(&desktop, 7);

    KTEST_EXPECT_EQ(tc,
                    desktop_write_process_output(&desktop, 8, "x", 1),
                    0);
}
```

- [ ] **Step 2: Run the kernel build to verify it fails before output routing exists**

Run: `make KTEST=1 kernel`

Expected: FAIL with missing declarations or undefined references for `desktop_attach_shell_pid` and `desktop_write_process_output`.

- [ ] **Step 3: Implement shell-surface stdout routing, kernel-log mirroring control, and boot wiring**

```c
/* kernel/gui/desktop.h */
void desktop_attach_shell_pid(desktop_state_t *desktop, uint32_t pid);
int desktop_write_process_output(desktop_state_t *desktop,
                                 uint32_t pid,
                                 const char *buf,
                                 uint32_t len);
int desktop_console_mirror_enabled(void);
```

```c
/* kernel/gui/desktop.c */
static int desktop_put_shell_char(desktop_state_t *desktop, char c)
{
    static int cursor_x;
    static int cursor_y;
    char text[2] = { c, '\0' };

    if (c == '\r')
        return 0;

    if (c == '\n' || cursor_x >= desktop->shell_content.w) {
        cursor_x = 0;
        if (cursor_y + 1 < desktop->shell_content.h) {
            cursor_y++;
        } else {
            for (int row = 1; row < desktop->shell_content.h; row++) {
                for (int col = 0; col < desktop->shell_content.w; col++) {
                    gui_cell_t src = gui_display_cell_at(
                        desktop->display,
                        desktop->shell_content.x + col,
                        desktop->shell_content.y + row);
                    gui_display_fill_rect(desktop->display,
                                          desktop->shell_content.x + col,
                                          desktop->shell_content.y + row - 1,
                                          1, 1, src.ch, src.attr);
                }
            }
            gui_display_fill_rect(desktop->display,
                                  desktop->shell_content.x,
                                  desktop->shell_content.y + desktop->shell_content.h - 1,
                                  desktop->shell_content.w, 1,
                                  ' ', 0x1e);
        }
    }

    if (c == '\n')
        return 0;

    gui_display_draw_text(desktop->display,
                          desktop->shell_content.x + cursor_x,
                          desktop->shell_content.y + cursor_y,
                          1, text, 0x1e);
    cursor_x++;
    return 0;
}

void desktop_attach_shell_pid(desktop_state_t *desktop, uint32_t pid)
{
    desktop->shell_pid = pid;
}

int desktop_write_process_output(desktop_state_t *desktop,
                                 uint32_t pid,
                                 const char *buf,
                                 uint32_t len)
{
    if (!desktop || !desktop->active || pid != desktop->shell_pid)
        return 0;

    for (uint32_t i = 0; i < len; i++)
        desktop_put_shell_char(desktop, buf[i]);

    gui_display_set_cursor(desktop->display,
                           desktop->shell_content.x,
                           desktop->shell_content.y,
                           1);
    gui_display_present_to_vga(desktop->display, 0xB8000u);
    return (int)len;
}

int desktop_console_mirror_enabled(void)
{
    return !desktop_is_active();
}
```

```c
/* kernel/proc/syscall.c */
#include "desktop.h"
...
        if (fh->type == FD_TYPE_STDOUT) {
            uint8_t kbuf[USER_IO_CHUNK];
            uint32_t written = 0;
            ...
                if (desktop_is_active() &&
                    desktop_write_process_output(desktop_global(),
                                                 cur->pid,
                                                 (const char *)kbuf,
                                                 chunk) == (int)chunk) {
                    written += chunk;
                    continue;
                }
                print_bytes((const char *)kbuf, (int)chunk);
                written += chunk;
            }
            return written;
        }
```

```c
/* kernel/lib/klog.c */
#include "desktop.h"
...
static void klog_puts(const char *s)
{
    if (desktop_console_mirror_enabled())
        print_string((char *)s);
    klog_debugcon_puts(s);
}
```

```c
/* kernel/kernel.c */
#include "desktop.h"
...
    static gui_cell_t desktop_cells[MAX_ROWS * MAX_COLS];
    gui_display_t boot_display;
    desktop_state_t *desktop = desktop_global();
    int desktop_ready = 0;
...
    if (mouse_init() == 0) {
        gui_display_init(&boot_display, desktop_cells, MAX_COLS, MAX_ROWS, WHITE_ON_BLACK);
        desktop_init(desktop, &boot_display);
        desktop_open_shell_window(desktop);
        desktop_render(desktop);
        desktop_ready = 1;
    } else {
        klog("GUI", "desktop disabled; using legacy console");
    }
...
    if (desktop_ready)
        desktop_attach_shell_pid(desktop, proc.pid);
```

- [ ] **Step 4: Run the kernel build again to verify boot/output routing compiles**

Run: `make KTEST=1 kernel`

Expected: PASS.

- [ ] **Step 5: Run full verification, including manual desktop bring-up**

Run: `make disk`

Expected: PASS.

Run: `make test`

Expected: KTEST completes, QEMU remains open, and `debugcon.log` shows no `desktop` suite failures.

Run: `pkill -f "qemu-system-i386 -drive format=raw,file=disk.img,if=ide,index=0"`

Expected: QEMU exits cleanly.

Run: `rg "desktop|FAIL" debugcon.log`

Expected: desktop tests pass and there are no new `KTEST FAIL` lines.

Run: `make run`

Expected: the system boots into a desktop-style text UI with a taskbar and an auto-opened shell window.

Manual checks:
- Press `Esc` to toggle the launcher.
- Press `Enter` in the launcher to reopen/refocus the shell.
- Type into the shell and confirm characters land inside the shell window.
- Click the taskbar menu area and shell window to confirm mouse focus changes.
- Temporarily disable desktop activation in `kernel/kernel.c` and confirm the legacy full-screen shell still boots.

- [ ] **Step 6: Commit the boot wiring and shell routing**

```bash
git add kernel/gui/desktop.h \
        kernel/gui/desktop.c \
        kernel/lib/klog.c \
        kernel/proc/syscall.c \
        kernel/kernel.c \
        kernel/test/test_desktop.c
git commit -m "feat: boot into desktop shell and route shell output"
```

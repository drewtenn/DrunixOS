# Pixel Terminal Desktop Design

Date: 2026-04-15
Status: Approved design

## Goal

Move the framebuffer desktop from a cell-presented shell window to a pixel-native terminal surface. The shell remains the main app, but framebuffer mode should draw the desktop and terminal as pixel geometry: window chrome, taskbar, launcher, terminal padding, glyph placement, cursor, pointer, and a visible scrollback cue.

This milestone should feel like a real pixel desktop without becoming a general graphical application framework. It should leave a clear boundary that later pixel apps can use, but the only client in this milestone is the shell terminal.

## Decisions

- Build a small pixel surface layer instead of a one-off renderer inside `desktop.c`.
- Make the shell terminal the first client of that pixel surface layer.
- Keep shell process behavior, TTY semantics, ANSI color handling, keyboard routing, and stdout/stderr routing intact.
- Add terminal scrollback in desktop mode, controlled by Page Up and Page Down.
- Draw a simple pixel scrollbar/thumb when terminal history exists.
- Use the "Warm Workbench" visual direction: readable padding, calmer contrast, and an underline cursor.
- Keep VGA fallback on the existing cell path.
- Keep mouse-wheel support, draggable scrollbars, overlapping windows, arbitrary fonts, anti-aliasing, and user-space graphics APIs out of scope.

## Architecture

The design splits framebuffer presentation into four pieces.

### Pixel Renderer Primitives

The framebuffer renderer should expose small deterministic drawing operations:

- fill a clipped rectangle
- draw an outlined or inset frame
- draw one glyph at any pixel coordinate
- draw a clipped text run
- draw a simple vertical scrollbar
- draw the existing pointer cursor on top of desktop contents

These operations stay low-level. They know about pixels, colors, clipping, and the framebuffer format, but they do not know about shell state or process ownership.

### Pixel Surface Boundary

Framebuffer mode should pass renderers a bounded pixel surface. A surface is a rectangle plus clipping information and a target framebuffer. It is not yet an app framework; it is only the reusable drawing contract that future pixel apps can share.

The first useful contract is:

- the desktop owns layout and assigns rectangles
- each surface renders inside its rectangle
- surface rendering must clip to that rectangle
- dirty regions are reported in pixel coordinates where practical

This gives later apps a place to plug in without requiring window movement, z-ordering, or compositor behavior now.

### Terminal Surface

The terminal surface moves shell-visible state out of `desktop.c` into a focused module under `kernel/gui`. It owns:

- live terminal rows
- retained history rows
- cursor position
- wrap-pending state
- ANSI color state
- current scrollback view
- dirty terminal region
- pixel geometry derived from its assigned surface rectangle

The terminal surface exposes operations for writing bytes, clearing, scrolling the view up or down, snapping back to live output, and rendering itself into a pixel surface.

### Desktop Pixel Path

`desktop_state_t` remains the top-level desktop owner. It still decides focus, launcher visibility, shell ownership, and input routing. In framebuffer mode, it should render:

- desktop background
- taskbar
- launcher
- shell window frame and title
- terminal surface
- terminal scrollbar
- pointer cursor

The existing `gui_display_t` path remains available for VGA fallback. Framebuffer mode should stop relying on full-desktop cell presentation as the primary renderer.

## Terminal Behavior

The terminal content area has pixel padding inside the shell window. Columns and rows are computed from the inner pixel rectangle after padding:

- `cols = inner_width / GUI_FONT_W`
- `rows = inner_height / GUI_FONT_H`

Shell text still uses the current `8x16` bitmap font. ANSI color meanings remain compatible with the current shell output, but foreground and background pixels are drawn by the terminal renderer instead of by presenting a full display cell grid.

The cursor is drawn as a pixel underline at the terminal cursor location while the terminal is live. When the user is viewing older history, the cursor is hidden or visually de-emphasized.

Page Up and Page Down move the terminal scrollback view. The scrollbar thumb represents the visible rows within the retained history plus live rows. New shell output snaps the view back to live and redraws the affected terminal region.

`clear` clears the live terminal surface, discards desktop terminal history for this first version, resets the cursor to the top-left of the terminal content, and returns the scrollback view to live.

## Shell And Syscall Integration

The existing console routing remains the entry point for shell output. Shell and foreground process output still flow through the desktop console path when the desktop owns the shell session.

`SYS_SCROLL_UP` and `SYS_SCROLL_DOWN` should affect the desktop terminal when the desktop is active. They should keep using the legacy VGA scrollback behavior when the desktop is inactive. This replaces the current framebuffer desktop behavior where those syscalls are ignored.

Keyboard input remains unchanged except for the scrollback syscalls taking effect in desktop mode. The shell's Page Up and Page Down handling can keep calling the same user-space wrappers.

## Error Handling And Fallback

The system must keep booting into a usable shell if the pixel terminal cannot initialize.

If framebuffer validation fails, Drunix continues using the VGA fallback path as it does today. If framebuffer mode is available but terminal buffer allocation fails, desktop initialization should fail cleanly and let the existing fallback path take over rather than presenting a broken shell window.

Renderer functions should clip all drawing to the framebuffer and assigned surface rectangles. Invalid or empty rectangles should become no-ops.

## Testing Strategy

Add focused kernel tests for the deterministic pieces:

- rectangle clipping and outline drawing bounds
- glyph drawing at non-cell-aligned pixel coordinates
- scrollbar thumb geometry
- terminal byte writing, wrapping, newline, backspace, tab expansion, and ANSI color application
- terminal clear behavior
- scrollback retention and Page Up/Page Down view movement
- snap-to-live on new output
- dirty region calculation
- framebuffer desktop using pixel terminal geometry
- VGA fallback still using the existing cell path
- launcher/menu redraw still working in framebuffer mode
- shell output routing still landing in the terminal surface
- `SYS_SCROLL_UP` and `SYS_SCROLL_DOWN` affecting the active desktop terminal

Build verification for the implementation should include `make kernel disk`. When practical, run a bounded QEMU smoke check or headless test target that can inspect `debugcon.log`; the ordinary `make test` target remains interactive because QEMU stays open.

## Documentation

When source changes land, update the existing framebuffer desktop documentation instead of describing this as a separate GUI system. The story should explain that Drunix still boots into the shell-first desktop, but framebuffer mode now renders the desktop and terminal through pixel surfaces while VGA fallback keeps the old cell path.

## Out Of Scope

- mouse-wheel scrollback
- draggable scrollbar thumbs
- arbitrary font loading
- anti-aliased text
- proportional fonts
- alpha blending or transparency
- overlapping windows
- compositor z-ordering
- user-space framebuffer access
- a general pixel app framework
- moving or resizing the shell window

## Open Follow-Ups

- mouse-wheel events if the PS/2 path grows wheel packet support
- draggable scrollbars
- text selection and copy behavior
- richer terminal cursor styles
- font loading or multiple bitmap fonts
- a small pixel app framework built on the surface boundary
- movable and resizable windows

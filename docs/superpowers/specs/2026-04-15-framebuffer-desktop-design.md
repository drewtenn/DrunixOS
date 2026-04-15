# Framebuffer Desktop Design

Date: 2026-04-15
Status: Approved design

## Goal

Take the current GUI desktop shell from VGA text presentation to a first framebuffer presentation while preserving the working shell-first desktop behavior.

The first framebuffer milestone should feel visibly upgraded, but it should not become a full pixel-native window manager. The shell remains the main app inside the GUI shell, with the existing taskbar/menu/window behavior, keyboard routing, stdout routing, ANSI handling, clear behavior, and mouse click behavior preserved.

## Decisions

- Use a hybrid framebuffer boot path: request `1024x768x32` from GRUB, validate what Multiboot provides, and fall back to VGA text if unsupported.
- Use a larger native logical grid in framebuffer mode. With an `8x16` bitmap font, `1024x768` yields `128x48` cells.
- Keep the desktop model cell-based for this milestone.
- Add a polished framebuffer renderer with RGB colors, title bars, panels, shell window styling, and a pixel arrow cursor.
- Use a pixel arrow cursor in framebuffer mode and the existing cell `^` cursor in VGA fallback mode.
- Keep arbitrary font loading, anti-aliasing, compositing, GPU acceleration, and user-space graphics APIs out of scope.

## Boot And Mode Selection

The kernel will extend its Multiboot1 integration to request a graphics framebuffer mode from GRUB:

- Width: `1024`
- Height: `768`
- Depth: `32`

The kernel must not assume the request succeeded. During boot it will validate the Multiboot framebuffer fields before enabling framebuffer presentation:

- framebuffer address is present and non-zero
- width and height are non-zero
- pitch is large enough for the mode
- bpp is `32`
- framebuffer type is direct RGB
- RGB mask positions and sizes are usable for packing pixels

If validation fails, the kernel keeps using the current VGA text desktop path. The boot log should make the selected display mode visible through `klog` or debugcon output, including fallback reason when practical.

## Display Architecture

`gui_display_t` remains the logical display buffer, but its dimensions become runtime-configured:

- VGA fallback: `80x25`
- Framebuffer mode: `framebuffer_width / 8` columns by `framebuffer_height / 16` rows

The display layer will gain a framebuffer presentation target alongside the existing VGA text target. The desktop should render into `gui_display_t` cells first, then the display backend presents those cells either as VGA text cells or as framebuffer pixels.

This keeps existing desktop tests meaningful and limits the first framebuffer implementation to display initialization, presentation, and adaptive layout.

## Framebuffer Renderer

The framebuffer renderer will live under `kernel/gui` and provide small, testable primitives:

- RGB pixel packing using Multiboot mask positions
- fill rectangular pixel regions
- draw bordered/title rectangles
- draw one `8x16` bitmap glyph
- draw strings
- present a `gui_display_t` cell buffer into the framebuffer
- draw a pixel arrow cursor after the desktop contents

The renderer will use a compiled-in `8x16` bitmap font. The font can initially cover the printable ASCII range used by the shell, kernel logs, frames, and menu labels.

The renderer should map existing VGA attributes to RGB colors, but framebuffer mode may use a richer theme than literal VGA colors. The target look is still simple and robust:

- dark desktop background
- brighter taskbar
- framed shell window with a visible title bar
- launcher panel
- readable shell text
- high-contrast pixel cursor

Transparency, anti-aliasing, proportional fonts, images, and animations are out of scope.

## Desktop Layout

The existing desktop layout must adapt to both `80x25` and `128x48` grids. Layout formulas should continue to derive taskbar, launcher, shell frame, and shell content from `display->cols` and `display->rows`.

In framebuffer mode, the larger grid should make the shell window visibly larger and more useful, not simply stretch an `80x25` experience. The shell content buffer should use the larger cell region, so commands can show more text without scrolling.

## Input Model

Keyboard behavior remains unchanged. The desktop keeps routing launcher/menu keys and forwarding shell text input as it does today.

Mouse movement should keep using the PS/2 packet path, but framebuffer mode will track pointer position in pixels for rendering. Click handling will map pointer pixels back to logical cells:

```text
cell_x = pixel_x / 8
cell_y = pixel_y / 16
```

The existing desktop hit tests can continue to use cell coordinates.

In VGA fallback mode, the pointer remains the existing cell-based `^` cursor.

## Shell Output And Console Behavior

The shell remains the main desktop app. Existing output routing should continue to work:

- shell stdout routes into the shell surface
- child process output routes into the shell surface
- legacy counted `SYS_WRITE` routes through the same console path
- `SYS_CLEAR` clears the shell surface in desktop mode
- scrollback syscalls remain no-ops in desktop mode until desktop scrollback exists
- ANSI color escapes used by the shell do not leak as raw text

Framebuffer mode should not introduce a second console path. It should present the same `gui_display_t` state through pixels.

## Fallback And Failure Handling

Fallback is required. DrunixOS must keep booting with the VGA text desktop if framebuffer setup fails.

Failure cases include:

- GRUB did not provide framebuffer info
- mode is not `32bpp`
- framebuffer type is unsupported
- pitch or dimensions are invalid
- framebuffer address cannot be represented safely by the current kernel mapping assumptions

The first milestone may rely on the current identity-mapped kernel address model if the framebuffer address is already accessible. If framebuffer mapping requires paging changes, that should be implemented explicitly and covered by tests or boot logs.

## Testing Strategy

Add kernel unit tests for deterministic pieces:

- Multiboot framebuffer parsing and validation
- `1024x768x32` grid calculation yields `128x48`
- unsupported/malformed framebuffer data falls back to VGA
- RGB pixel packing from mask positions
- fill rectangle clips to framebuffer bounds
- glyph rendering writes expected foreground/background pixels
- framebuffer presentation maps `gui_cell_t` characters and attributes to pixels
- desktop layout remains valid at `80x25`
- desktop layout remains valid at `128x48`
- framebuffer cursor rendering is separate from VGA cell cursor behavior

Continue using QEMU smoke tests:

- `make KTEST=1 kernel`
- `make disk`
- `make test`, then manually stop QEMU
- inspect `debugcon.log` for KTEST pass, selected display mode, and boot to ring 3
- `make kernel`
- `make run`, then manually stop QEMU

## Out Of Scope

- VESA BIOS calls from the kernel
- mode switching after boot
- arbitrary resolution selection UI
- GPU acceleration
- compositing/window stacking
- transparent or alpha-blended windows
- anti-aliased fonts
- PSF/font file loading
- user-space framebuffer or graphics APIs
- converting the shell into a pixel-native terminal widget

## Open Follow-Ups After This Milestone

- Desktop scrollback in framebuffer mode
- Richer font support
- Pixel-native widgets
- User-space graphics APIs
- Better pointer shapes and cursor save/restore optimization
- Real window movement/resizing

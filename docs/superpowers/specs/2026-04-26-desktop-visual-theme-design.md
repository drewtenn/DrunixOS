# Desktop Visual Theme Design

## Goal

Update the framebuffer desktop UI so it visually resembles the supplied
Windows-like dark glass desktop screenshot while preserving Drunix's existing
desktop functionality.

This is a visual theme change only. The implementation must not add fake
Browser, Notes, Clock, or Settings apps, must not rename existing apps to imply
missing functionality, and must not create shortcuts that launch unavailable
features.

## Current State

Drunix already has a framebuffer desktop compositor with:

- draggable windows, focus, z-order, close buttons, and taskbar switching;
- a shell terminal window;
- Files, Processes, and Help mini-app windows;
- an Escape launcher;
- a text-mode VGA fallback path.

The current framebuffer palette is warm and retro: green desktop background,
light taskbar, yellow borders, and red title bars. The requested direction is a
cooler blue desktop with dark glass-like panels and brighter blue accents.

## Scope

The change applies to the framebuffer desktop path in `kernel/gui/desktop.c`
and supporting tests in `kernel/test/test_desktop.c`.

The text/VGA fallback keeps its existing character-cell style unless a small
constant update is required to preserve current behavior.

## Visual Direction

The framebuffer theme will use:

- a dark blue desktop base;
- soft blue and teal diagonal wallpaper bands drawn directly into the
  framebuffer;
- a dark bottom taskbar with subtle top highlight and blue active accents;
- dark window bodies with muted blue-gray title bars;
- lighter title text and terminal text;
- restrained blue outlines and focus indicators;
- deterministic pixel drawing that suggests glass through layered colors,
  highlights, and shadows.

No alpha blending or fake transparency is required. The compositor should draw
opaque colors in a deliberate order, as it does today.

## Components

### Desktop Background

Replace the flat framebuffer desktop fill with a small wallpaper renderer. It
fills the clip with a dark blue base, then draws broad diagonal bands using
bounded pixel loops or rectangle helpers. The renderer must clip all writes to
the dirty region and framebuffer bounds.

### Windows

Keep the existing window model and hit testing. Restyle the framebuffer chrome:

- title bars become dark blue-gray;
- window borders become subtle blue outlines;
- focused windows receive a brighter accent;
- close buttons remain visible and clickable;
- content rectangles and terminal geometry remain stable.

### Taskbar

Keep the taskbar at the bottom and preserve taskbar app hit testing. Restyle it
as a dark dock-like strip with a top highlight, launcher area, open-window
labels, and focused-window accent.

### Launcher

Keep Escape and taskbar launcher behavior. Restyle the launcher as a dark
floating panel with blue selected-row treatment. The launcher still contains
only the real Drunix entries: Shell, Files, Processes, and Help.

## Data Flow

Pointer and keyboard event routing remain unchanged. Rendering changes stay
inside the compositor drawing path:

1. dirty region is selected;
2. wallpaper/background is drawn into that region;
3. windows are drawn in z-order;
4. taskbar and launcher are drawn on top;
5. the dirty region is presented.

The shell terminal continues to receive process output through the existing
desktop terminal path.

## Error Handling

Rendering helpers must tolerate null pointers and empty or out-of-bounds clips
the same way existing framebuffer helpers do. Geometry clamping, dirty-region
present logic, and framebuffer critical sections remain unchanged.

## Testing

Use test-first changes for observable behavior. Add or update kernel desktop
tests that verify:

- representative framebuffer background pixels use the new blue theme;
- taskbar pixels use the dark taskbar theme while taskbar hit testing still
  focuses open windows;
- window chrome pixels use the new dark title/border colors while close-button
  tests still pass;
- launcher item row mapping still opens the same real apps after the visual
  restyle.

Existing desktop and terminal tests must continue to pass.

## Non-Goals

- Adding Browser, Notes, Clock, Settings, or system tray functionality.
- Renaming existing apps to mimic missing apps.
- Implementing real transparency, blur, or compositing effects.
- Changing shell process ownership, terminal behavior, app semantics, or input
  routing.

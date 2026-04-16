# Windowed Mini Apps Design

Date: 2026-04-16
Status: Approved design

## Goal

Phase IX turns the framebuffer desktop from a shell-centered surface into a small
windowed environment with built-in mini apps. The shell remains the anchor app,
but Drunix should also let the user open, move, focus, close, and return to a
few simple app windows from the launcher and taskbar.

The first app bundle is **Explore The System**:

- Files
- Processes
- Help

The experience should make the OS feel more discoverable without building a
general user-space GUI platform yet.

## Decisions

- Implement real movable windows, not fixed mock windows.
- Keep the window manager inside the existing desktop subsystem.
- Keep mini apps kernel-owned built-in desktop views for this phase.
- Support open, close, focus, raise, taskbar selection, and title-bar dragging.
- Ship the first bundle as Files, Processes, and Help.
- Keep resizing, minimizing, app-to-app IPC, user-space GUI APIs, and arbitrary
  third-party graphical apps out of scope.
- Keep the shell special only where it must remain tied to the live terminal and
  process-output routing.

## Architecture

The framebuffer desktop gains a small internal window manager. It owns a fixed
table of desktop windows. Each window has:

- a stable id
- a title
- an app kind
- an open/focused state
- a pixel rectangle
- z-order information
- optional per-app state

The existing shell window becomes one window in that table, backed by the
current `gui_terminal_t` terminal path. Files, Processes, and Help use the same
window chrome and taskbar behavior, but render static or refreshed app content
inside their assigned content rectangles.

This phase should build on `desktop_state_t`, which already owns focus, the
launcher, shell geometry, pointer input, framebuffer state, and the shell
terminal. The design generalizes that structure from one shell window to a
small bounded set of windows.

## Components

### Window Manager Layer

The desktop subsystem owns window allocation, close behavior, focus, z-order,
drag state, title-bar hit testing, close-button hit testing, taskbar button hit
testing, and clamping. It remains an internal kernel GUI layer, not a separate
process or server.

### Mini App Interface

Each mini app exposes a small internal interface:

- render into a clipped pixel content rectangle
- handle keyboard input when focused
- handle pointer input inside its content area
- refresh its model when opened or when explicitly requested

Apps do not draw directly to the framebuffer. The desktop passes them a content
surface clipped to the window body.

### Explore Apps

The first app kinds are:

- **Files:** list entries for the selected directory, starting at `/`.
- **Processes:** show a bounded process snapshot: PID, name, state, parent,
  process group, and a small memory or status summary when available.
- **Help:** show built-in help pages for navigation, commands, keyboard
  controls, files, processes, and desktop controls.

App state should stay small and bounded. Prefer fixed-size arrays and short text
buffers over heap-heavy UI trees or a general widget toolkit.

### Launcher And Taskbar

The launcher opens Shell, Files, Processes, and Help. If an app window is
already open, selecting it should focus and raise the existing window rather
than creating duplicates. This phase supports one instance of each built-in app.

The taskbar shows open windows. Selecting a taskbar button focuses and raises
that window.

## Behavior

### Pointer Input

- Clicking a title bar focuses and raises the window and starts drag tracking.
- Moving the pointer while dragging updates the window rectangle.
- Releasing the pointer ends the drag.
- Dragging clamps windows so the title bar remains reachable and the taskbar is
  not covered permanently.
- Clicking a close button closes the window and resets its app state if needed.
- Clicking a taskbar button focuses and raises that window.
- Clicking inside a window body focuses the window and forwards the event to the
  app.

### Keyboard Input

Shell-focused keyboard input keeps using the shell terminal path.

Files, Processes, and Help use a small first-pass key set:

- Up and Down move selection or scroll where applicable.
- Page Up and Page Down scroll longer views.
- Enter opens or activates the selected item when meaningful.
- Escape or `q` closes the focused mini app window.

Global shortcuts, including Alt+Tab, are out of scope for the first pass.

### Rendering

The desktop renders in this order:

1. background
2. taskbar and launcher
3. windows from back to front
4. window chrome for each window
5. app content inside each clipped content rectangle
6. mouse pointer

The shell terminal renderer continues to render the shell content area. Mini
apps render through the same pixel-surface discipline rather than writing
directly to the framebuffer.

Dirty rectangles are preferred where practical. A full framebuffer repaint after
open, close, focus, or drag is acceptable for the first implementation if it
stays responsive in QEMU.

### App Refresh

Files refreshes when opened, when changing directories, and when explicitly
reloaded.

Processes refreshes when opened and on user interaction. If a low-cost desktop
timer refresh already exists by implementation time, Processes may use it, but
the first version does not require a new timer policy.

Help uses static compiled-in text.

## Error Handling

If framebuffer initialization fails, Drunix keeps using the existing VGA and
shell fallback.

If a mini app cannot load its data, the window opens with a short error message
inside the content area. The whole desktop should not fail because one app could
not refresh.

If the window table is full, launcher activation should avoid corrupting state.
It may ignore the request or show a small status message.

If Files sees more directory entries than its buffer can hold, it shows the
first page and indicates that more entries exist.

If a process exits while Processes is rendering, the app tolerates stale or
missing data and refreshes cleanly on the next pass.

Renderer and app functions must clip to the assigned window/content rectangle.
Invalid or empty rectangles are no-ops.

## Testing Strategy

Add focused KTEST coverage for:

- window allocation and close behavior
- focus, raise, and z-order updates
- title-bar and close-button hit testing
- taskbar hit testing and focus behavior
- drag start, drag movement, drag end, and clamping
- launcher opening Shell, Files, Processes, and Help
- app render clipping into content rectangles
- Files directory listing and overflow behavior
- Processes snapshot formatting and stale-process tolerance
- Help page rendering and scrolling
- shell output still routing to the shell window after other windows open
- pointer rendering on top of windows
- framebuffer desktop fallback still leaving VGA shell behavior usable

Build verification for implementation should include `make kernel disk`.
When practical, run the in-kernel test flow with `make test` or the repository's
current KTEST target.

## Documentation

Update the desktop-related docs when implementation lands. The story should
present Phase IX as Drunix's move from a shell-first framebuffer desktop to a
small discoverable windowed environment. It should not describe the feature as a
general-purpose GUI server or user-space app platform.

## Out Of Scope

- resizable windows
- minimize/restore behavior
- Alt+Tab or global window shortcuts
- overlapping window transparency
- user-space framebuffer access
- user-space GUI APIs
- multiple instances of the same mini app
- arbitrary third-party graphical apps
- app-to-app IPC
- persistent window placement
- settings persistence
- text editing inside mini apps

## Open Follow-Ups

- minimize/restore
- resize handles
- app event queues
- a user-space GUI protocol
- persistent desktop settings
- text viewer/editor app
- kernel log and core dump mini apps
- richer process memory views
- file open actions from Files

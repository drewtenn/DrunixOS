# GUI Desktop Shell Design

Date: 2026-04-14
Status: Approved for planning

## Summary

Build the first GUI desktop shell for DrunixOS as a text-mode-first system that
keeps a clean upgrade path to a future pixel framebuffer backend. The first
release should present a desktop frame with a taskbar, launcher/menu, focus
states, and a hosted shell window that opens automatically at boot. The shell
remains the primary useful app, but it now runs inside desktop-managed chrome
instead of owning the full screen directly.

The design intentionally separates desktop behavior from the current VGA text
implementation. Milestone one will still render through the existing text-mode
stack, but the desktop shell must depend on backend-neutral display and input
interfaces so the same desktop model can later move to a pixel renderer without
rewriting the shell host, launcher, focus rules, or app-surface contracts.

## Goals

- Make DrunixOS boot into a visible desktop shell instead of a bare full-screen
  console.
- Keep the existing shell as the main hosted app, launched from the taskbar or
  launcher and auto-opened at boot in the first milestone.
- Support basic keyboard and mouse interaction from day one.
- Preserve a clean path from text-mode rendering to a future pixel-based GUI
  backend.
- Contain the first release to a focused shell-desktop milestone rather than a
  full desktop environment.

## Non-Goals

- Full overlapping window management for arbitrary apps.
- A file manager, settings app, or general desktop application framework.
- Rich theming, icons, wallpaper assets, or advanced visual polish.
- Replacing the current shell implementation with a new command interface.
- Shipping a production-quality pixel graphics stack in the first milestone.

## User Outcomes

After this milestone lands, a user booting DrunixOS should see:

- desktop chrome instead of a plain console takeover
- a visible taskbar and launcher/menu affordance
- an auto-opened shell window inside the desktop shell
- enough keyboard and mouse support to move focus, open the launcher, and
  activate the hosted shell

If the desktop shell cannot initialize, the machine should still remain usable
by falling back to the current full-screen text shell.

## Scope

The first release covers:

- a desktop shell that owns the full screen after boot
- a text-mode display backend that renders desktop regions inside the existing
  VGA text environment
- a backend-neutral display interface that can later support a pixel renderer
- a basic input-event layer for keyboard and mouse events
- a launcher/menu with at least one primary action: open or focus the shell
- a hosted shell surface displayed inside desktop-managed chrome
- boot flow that shows the desktop and auto-opens the shell window
- fallback to the current text shell if desktop initialization fails or is
  disabled

The first release must not require general multi-window support. It may include
only one primary hosted shell surface plus desktop chrome, as long as the
surface abstraction is designed so later hosted apps can follow the same model.

## Current Constraints

The design must fit the current DrunixOS hardware and runtime state:

- output today is VGA text mode, not a pixel framebuffer
- keyboard and TTY plumbing already exist
- the current shell assumes it is talking to the terminal directly
- there is no existing mouse driver or general GUI event system yet

These constraints favor a staged design: keep milestone one visually simple,
but avoid baking VGA-memory assumptions into the desktop shell itself.

## Architecture

The GUI shell is split into three layers.

### 1. Display Backend

The display backend owns low-level drawing and presentation. In milestone one,
this backend targets VGA text mode and is allowed to use cell-oriented drawing
internally. It should expose neutral operations such as:

- clear or fill a bounded region
- draw text into a region
- draw borders or simple chrome
- place or hide a pointer/cursor indicator
- present changed regions

The desktop shell must not depend directly on VGA memory layout, cursor port
registers, or full-screen console internals.

### 2. Desktop Shell

The desktop shell becomes the top-level screen owner once boot reaches the
handoff point where the shell would normally take over the console. It manages:

- desktop layout
- taskbar and launcher visibility
- focus state
- shell launching and focusing
- routing input between global chrome and the focused app surface
- redraw requests for dirty regions

Milestone one should keep this logic close enough to the current console path
that it can reuse existing boot and TTY behavior, but it should still be
structured as a distinct subsystem rather than scattered through the keyboard
driver or shell code.

### 3. App Surface Adapter

The hosted shell must run through an adapter that bridges TTY-style shell I/O
into a bounded desktop region. This adapter is the first example of the app
surface contract. It should:

- define the rectangle owned by the hosted shell
- accept text output destined for that region
- receive focus-aware input events
- notify the desktop shell when its visible content changes

The app surface model is the main seam that should survive the future move from
text mode to pixel mode.

## Layout Model

The first milestone layout should stay fixed and simple:

- a desktop background region
- a taskbar region that remains visible
- a launcher/menu anchored from the taskbar
- one shell window region with lightweight frame chrome

At boot, the desktop shell should draw the desktop, show the taskbar, and
automatically open the shell window so the system is immediately useful. The
launcher should still expose the shell as its main app entry so the user can
close and reopen or refocus it through desktop controls.

The initial shell window does not need freeform dragging or resizing. A fixed
or tightly constrained placement is acceptable in milestone one.

## Input Model

Milestone one must support hybrid input from day one.

### Keyboard

Keyboard input should support:

- desktop-global shortcuts such as opening or dismissing the launcher
- focus movement between desktop chrome and the shell surface
- activation keys such as Enter and Escape inside menus
- ordinary typed input forwarded to the focused shell surface

### Mouse

The system should add basic mouse support sufficient for:

- moving a visible pointer or cell-granularity selector
- clicking the launcher/menu affordance
- focusing or activating the shell surface
- selecting launcher items

The first release does not require advanced pointer interactions such as
dragging or resize handles. In text mode, the pointer may be represented by a
text-cell highlight, inverted cell, or other simple indicator as long as it is
visibly distinct and routed through the same backend-neutral input model that a
pixel cursor would use later.

## Data Flow

Boot should proceed much as it does today until the shell handoff point. At
that moment:

1. the desktop shell initializes its display backend
2. the desktop layout is drawn
3. the launcher/taskbar chrome becomes active
4. the shell surface is created and auto-opened
5. the desktop shell enters its normal event-routing loop

Input should flow through a central dispatcher:

- desktop-global events are handled by the desktop shell
- focused-surface events are routed to the shell adapter
- launcher-visible navigation events are temporarily routed to the launcher

Rendering should be event-driven. The desktop shell should track which regions
changed and request redraw only for those regions. The first text-mode backend
may internally repaint at row granularity if that keeps the implementation
simple, but the interface must treat redraws as bounded-region updates rather
than permanent full-screen ownership.

## Shell Integration

The existing shell remains the core interactive app in the first milestone. The
design must preserve current shell behavior wherever possible, especially:

- TTY-backed input semantics
- prompt, command execution, and output behavior
- job-control expectations already present in the shell and TTY stack

What changes is ownership of the visible output surface. The shell should no
longer assume it controls the entire visible console. Instead, its terminal
output must be clipped or adapted into the shell window region managed by the
desktop shell.

The shell should also remain launchable from the desktop menu even though it
auto-opens at boot. That keeps the launcher meaningful and establishes the
pattern for later hosted apps.

## Failure Handling And Fallback

Desktop-shell bring-up must fail safely.

- If desktop initialization fails, DrunixOS must fall back to the current
  full-screen text shell.
- If mouse initialization fails, the desktop should remain usable with keyboard
  navigation only where practical.
- If the launcher or shell surface enters an invalid state, the desktop shell
  should prefer redrawing or relaunching the hosted shell over leaving the
  whole screen corrupted.

The fallback path is a product requirement, not just a debug convenience. Early
desktop work should never strand the system in an unusable boot state.

## Testing Strategy

Testing should target subsystem seams rather than visual appearance alone.

### Display Tests

Use fake or test backends to verify:

- region clipping
- text rendering into bounded rectangles
- border and chrome drawing behavior
- dirty-region invalidation and presentation ordering

### Desktop-Shell Tests

Add focused tests for:

- initial layout state
- shell auto-launch at boot
- launcher open, close, and selection behavior
- focus movement between desktop chrome and shell surface
- fallback behavior when desktop initialization fails

### Input Tests

Add tests for:

- keyboard event routing to global chrome versus focused surface
- basic mouse event decoding and dispatch
- launcher activation through both keyboard and mouse input

### Manual Bring-Up Checks

The first acceptance pass should confirm:

- the system boots to a desktop view
- the taskbar and launcher affordance are visible
- the shell window opens automatically
- the shell can be focused from both keyboard and mouse interactions
- the launcher can reopen or refocus the shell
- disabling or breaking the desktop path still leaves a usable text shell

## Milestone Boundary

The milestone is complete when DrunixOS boots into a desktop shell rendered in
text mode, supports basic keyboard and mouse interaction, hosts the existing
shell inside desktop-managed chrome, and retains a working fallback to the
legacy full-screen console path.

It is not necessary for milestone one to solve arbitrary windows, pixel
graphics, theming, or a broader app ecosystem. Success is a stable and usable
desktop shell foundation with clean interfaces for future growth.

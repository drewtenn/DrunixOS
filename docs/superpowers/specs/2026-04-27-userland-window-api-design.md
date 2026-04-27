# Userland Window API Design

## Goal

Add a Win32-like window API for Drunix userland applications. A normal app
should be able to create a desktop window, map a pixel surface for that
window, draw into it, present dirty rectangles, and receive input and lifecycle
events without owning the real framebuffer or keyboard and mouse devices.

The desktop remains the compositor, launcher, taskbar, and window policy owner.
The kernel only brokers connections, ownership, event queues, and mmap-able
window backing pages. Desktop applications, including Terminal, Files,
Processes, and Help, run as separate client processes using the same public
window API as third-party apps.

## Architecture

The system will add a narrow kernel broker exposed as `/dev/wm`. The existing
`desktop` user-space process opens this device as the single window-manager
server after it claims the display. Application processes open the same device
as clients.

The kernel broker is responsible for:

- tracking which process owns each window handle
- allocating and mapping each client window's pixel backing store
- forwarding client requests to the desktop server
- forwarding desktop events to the owning client
- cleaning up a client's windows when its file descriptor or process exits

The broker does not draw title bars, choose focus, manage z-order, launch apps,
or composite pixels into `/dev/fb0`. Those decisions stay in
`user/apps/desktop.c`, where the taskbar, launcher, focus, drag, minimize,
close, input routing, and framebuffer presentation policy belongs. Application
content does not live in `desktop`; every app window is client-owned.

## Public API

User programs will include `user/runtime/drwin.h`, backed by
`user/runtime/drwin.c`. Apps use that wrapper instead of writing raw `/dev/wm`
request records.

The first public surface is intentionally small:

```c
typedef int drwin_window_t;

typedef struct {
	int x;
	int y;
	int w;
	int h;
} drwin_rect_t;

typedef struct {
	int width;
	int height;
	int pitch;
	int bpp;
	void *pixels;
} drwin_surface_t;

typedef enum {
	DRWIN_EVENT_NONE,
	DRWIN_EVENT_CLOSE,
	DRWIN_EVENT_KEY,
	DRWIN_EVENT_MOUSE,
	DRWIN_EVENT_FOCUS,
	DRWIN_EVENT_RESIZE
} drwin_event_type_t;

typedef struct {
	drwin_event_type_t type;
	drwin_window_t window;
	int x;
	int y;
	int code;
	int value;
} drwin_event_t;
```

Initial calls:

```c
int drwin_connect(void);
int drwin_create_window(const char *title,
                        int x,
                        int y,
                        int w,
                        int h,
                        drwin_window_t *out);
int drwin_map_surface(drwin_window_t window, drwin_surface_t *out);
int drwin_present(drwin_window_t window, drwin_rect_t dirty);
int drwin_poll_event(drwin_event_t *event, int timeout_ms);
int drwin_set_title(drwin_window_t window, const char *title);
int drwin_show_window(drwin_window_t window, int visible);
int drwin_destroy_window(drwin_window_t window);
```

The first version will not add menus, child controls, timers, icons, cursor
customization, or drawing helpers. Applications draw pixels directly into their
mapped surfaces and call `drwin_present()` with a dirty rectangle.

## Device Protocol

`/dev/wm` will behave like the existing character devices where possible. It
needs `read`, `write`, `poll`, and `mmap` operations.

Client flow:

1. An app opens `/dev/wm`.
2. The app writes a create-window request with title and initial geometry.
3. The kernel allocates a window id, owner process id, event queue, and backing
   pages.
4. The app maps the window surface with `mmap()`.
5. The app draws into the mapped pixels.
6. The app writes a present request with a dirty rectangle.
7. The desktop server receives the present notification and composites the
   dirty region into the real framebuffer.
8. The app reads or polls `/dev/wm` for input and lifecycle events.

Server flow:

1. `desktop` opens `/dev/wm`.
2. `desktop` sends a register-server request.
3. The kernel accepts exactly one active server.
4. `desktop` reads broker messages for create, destroy, present, title, and
   visibility changes.
5. `desktop` updates z-order, focus, drag, taskbar, minimize, close, and dirty
   composition state.
6. `desktop` sends events back to the owning client window.

## Window Surfaces

Each client window owns one pixel backing store. The initial implementation
should use 32-bit pixels to match the current desktop's internal scene and
framebuffer assumptions. The mapped surface reports width, height, pitch, and
bits per pixel so the API can survive future framebuffer layout changes.

The broker allocates enough pages for the requested pitch times height, rounded
up to a page boundary. The app maps those pages through `/dev/wm` using an
offset that identifies the window. The exact offset encoding is private to the
runtime wrapper and broker; applications should treat `drwin_map_surface()` as
the contract.

Dirty rectangles are advisory. The desktop clips them to the window content
area and screen bounds before copying from the client surface into its scene.

## Events

Each window gets an event queue owned by its client. Events should include:

- close request from the title-bar close button
- key press for the focused window
- mouse move and button events inside the content area
- focus gained or lost
- resize notification if resizing is added to the compositor
- disconnect notification if the desktop server exits

The first version can route close, key, mouse, and focus events. Resize can be
reserved in the ABI so callers can compile against it before interactive resize
exists.

`drwin_poll_event()` should block, return immediately, or time out using the
same timeout convention as `sys_poll()`: negative means wait indefinitely, zero
means non-blocking, and positive values are milliseconds.

## Error Handling

All public calls return `0` on success or `-1` on failure unless they naturally
return a count or descriptor. Handles are per-client and cannot be used by
other processes.

Failure cases:

- invalid window handles fail
- clients cannot operate on windows they do not own
- oversized windows fail with `-1`; the first implementation caps client
  surfaces at 1024 by 768 pixels
- `drwin_create_window()` fails when the desktop server is not registered
- client exit destroys that client's windows and notifies the server
- desktop exit sends a disconnect event and causes future client operations to
  fail
- title strings are copied into kernel-owned fixed-size storage
- malformed request records are rejected without changing state

## Integration With The Existing Desktop

The current desktop has hard-coded built-in windows for Terminal, Files,
Processes, and Help. The implementation should remove those built-in window
paths. Desktop-owned UI is limited to compositor chrome, pointer rendering,
taskbar, clock, and launcher behavior. Terminal, Files, Processes, and Help
become normal userland apps that connect to `/dev/wm`, create windows, draw
into mapped buffers, and receive input through their own event queues.

The taskbar should launch `/bin/terminal`, `/bin/files`, `/bin/processes`, and
`/bin/help` as separate processes. Each launched app owns its window. Z-order
and focus should apply only to client windows, so clicking a window raises it,
routes keyboard events to its owner process, and draws it above lower windows.

The Terminal app owns the pseudo-terminal session and terminal emulator. It
uses the public `drwin` API for its window and maps keyboard events from the
desktop into the pty master. The desktop no longer starts the shell directly or
contains terminal grid/rendering state.

If this conversion needs OS functionality that is missing, such as reliable app
launch from the compositor, event polling on `/dev/wm`, or per-window buffer
mapping, the implementation adds that functionality first. It must not bypass
the framework with private desktop-only paths.

## Testing

The first implementation should be test-driven around the broker data model,
the user-facing API, and the removal of built-in desktop windows. Kernel tests
should cover handle allocation, ownership checks, event queue delivery, server
exclusivity, cleanup on close, and mmap metadata. Userland policy tests should
ensure the runtime exposes the public header, client apps build against it, and
`desktop.c` no longer contains Terminal, Files, Processes, or Help rendering
paths.

Targeted validation should include:

- a failing-then-passing broker unit test for client window creation
- a failing-then-passing ownership test for cross-process handle rejection
- a failing-then-passing event queue test for close and key events
- a failing-then-passing desktop policy check proving built-in app window paths
  are gone
- a failing-then-passing build check for `/bin/terminal`, `/bin/files`,
  `/bin/processes`, and `/bin/help` as `drwin` client apps
- `make -C user print-progs`
- `python3 tools/test_user_desktop_window_framework.py`
- `python3 tools/check_userland_runtime_lanes.py`

Full graphical boot testing can follow once the broker, runtime wrapper,
desktop server, and client-owned Terminal, Files, Processes, and Help apps are
wired together.

\newpage

## Chapter 28 - Desktop

### From Framebuffer Process to Window Server

The desktop is now a user-space compositor, not a collection of built-in
application windows. It owns the display, registers as the central `/dev/wm`
server, launches applications as separate processes, maps their window
surfaces, and routes input back to the owning clients. Terminal, Files,
Processes, and Help are normal userland programs under `/bin`; the desktop
does not render their contents itself.

Startup follows this path:

1. Read `/dev/fb0info` for framebuffer geometry and channel layout.
2. Map `/dev/fb0` and allocate a scene buffer plus wallpaper buffer.
3. Claim the display with `sys_display_claim()`.
4. Open `/dev/wm` and send `DRWIN_REQ_REGISTER_SERVER`.
5. Start keyboard and mouse helper processes.
6. Render the desktop shell, then launch `/bin/terminal` as the first client.

This keeps ownership boundaries simple. The desktop owns the framebuffer and
global policy. Each application owns its process, window, and pixel surface.
The kernel window broker owns the API boundary between them.

### The Window Broker

Applications use the `drwin` runtime to create windows. A client opens
`/dev/wm`, sends `DRWIN_REQ_CREATE_WINDOW`, maps the returned surface offset,
draws into that memory, and presents dirty rectangles with
`DRWIN_REQ_PRESENT_WINDOW`.

The desktop connects to the same device as the server. The broker delivers
fixed-size `drwin_server_msg_t` records for:

- window creation,
- window destruction,
- surface presents,
- title changes,
- visibility changes, and
- server disconnects.

On create, the desktop allocates a compositor slot, maps the client's surface
through `/dev/wm`, stores the title and geometry, focuses the new window, and
repaints. If it cannot track or map a new client window, it sends
`DRWIN_EVENT_CLOSE` back to that window instead of leaving an invisible orphan.

### Client-Owned Apps

The built-in desktop app model is gone. The taskbar launches these processes:

- `/bin/terminal`
- `/bin/files`
- `/bin/processes`
- `/bin/help`

Taskbar clicks first look for an existing matching client window. If one
exists, the desktop restores and focuses it. Only when no matching window is
present does it fork and exec a new app. Launched children close inherited
desktop-only file descriptors before `exec`, including the server-side
`/dev/wm` fd, so the compositor remains the single server owner.

Terminal is also a client. It allocates a PTY through `/dev/ptmx`, discovers
the matching slave with `TIOCGPTN`, forks `/bin/shell`, renders PTY output into
its own `drwin` surface, forwards keyboard bytes to the PTY master, and reaps
the shell on shutdown.

### Rendering

The compositor still uses an off-screen scene buffer. A frame is built from:

1. the procedural wallpaper,
2. client windows in z order,
3. shared title-bar chrome around each client surface,
4. the taskbar, and
5. the cursor overlay in the live framebuffer.

Client pixels are copied from mapped surfaces into the window content area.
The desktop draws only chrome and global UI. It never knows whether a client is
Terminal, Files, Processes, Help, or a future app except for taskbar title
matching.

Dirty presents are clipped against the screen and unioned with the old and new
cursor rectangles. If a partial compose cannot be satisfied, the compositor
falls back to a full scene present.

### Input and Focus

Keyboard and mouse input are read by helper processes and forwarded to the
desktop over a small pipe. The desktop then applies window-manager policy:

- taskbar clicks launch, restore, or focus apps;
- title-bar clicks focus, drag, minimize, or close windows;
- close buttons send `DRWIN_EVENT_CLOSE` to the owning client;
- focus changes send `DRWIN_EVENT_FOCUS`;
- keyboard bytes go only to the visible focused client; and
- pointer motion/button events go to the client under the pointer body.

Minimized or hidden windows cannot keep keyboard focus. When a focused window
is minimized or hidden, focus is cleared until the user selects another
visible client.

### Process and Lifetime Rules

The desktop tracks launched app pids and reaps them with a wait helper that
returns the actual waited pid separately from the encoded exit status. This is
important because an app that exits with status 0 must still free its launcher
slot.

Window lifetime is brokered through `/dev/wm`. When a client destroys its
window or exits, the broker queues a destroy message for the server. The
desktop unmaps the surface, clears its compositor slot, repaints the damaged
region, and drops focus/drag state if needed.

### Where the Machine Is by the End of Chapter 28

Drunix now has a user-space desktop service with client-owned application
windows. The compositor is a central window server and launcher. Apps are
separate processes using the same `drwin` API future userland programs can use,
and built-in desktop windows have been removed from the compositor itself.

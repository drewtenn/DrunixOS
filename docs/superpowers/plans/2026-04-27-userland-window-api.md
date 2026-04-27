# Userland Window API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a central desktop window service so separate userland processes, including Terminal, Files, Processes, and Help, can create windows, map per-window pixel buffers, present dirty rectangles, and receive window events.

**Architecture:** Add `/dev/wm` as a kernel-brokered device with per-open window-manager connections. The kernel tracks ownership, event queues, and per-window physical pages; `desktop` registers as the only server and remains responsible for composition, focus, title bars, taskbar, launcher, process launching, and input routing. All app content lives in client-owned windows; the desktop must not keep built-in Terminal, Files, Processes, or Help window paths.

**Tech Stack:** Drunix kernel C, KTEST, VFS/devfs, fd table resources, `mmap`, user runtime C, ptys, user-space desktop compositor.

---

## File Structure

- Create `shared/wm_api.h`: stable user/kernel wire structs, request opcodes, event types, limits, and pixel format constants.
- Create `kernel/drivers/wmdev.h`: kernel-local broker API used by fd, mmap, and tests.
- Create `kernel/drivers/wmdev.c`: `/dev/wm` connection table, window table, event queues, page allocation, request dispatch, and test helpers.
- Modify `kernel/fs/vfs/core.c`: add `wm` to the devfs static entries.
- Modify `kernel/proc/process.h`: add `FD_TYPE_WM` and a `wm.conn_id` fd union member.
- Modify `kernel/proc/syscall/vfs/open.c`: special-case `/dev/wm` char device opens through `wmdev_open()`.
- Modify `kernel/proc/syscall/helpers.c` and `kernel/proc/resources.c`: release WM connections on close/exit/forked fd-table cleanup.
- Modify `kernel/proc/syscall/fd.c`: route `read()` and `write()` for `FD_TYPE_WM`.
- Modify `kernel/proc/syscall/fd_control.c`: report `POLLIN` for queued WM events and `POLLOUT` for writable WM connections.
- Modify `kernel/proc/syscall/mem.c`: support non-contiguous per-page mapping for `FD_TYPE_WM` window surfaces.
- Modify `kernel/objects.mk` and `kernel/arch/arm64/arch.mk`: compile `wmdev.c` for x86 and arm64.
- Modify `kernel/arch/x86/start_kernel.c`: initialize the WM broker after devfs-supporting drivers.
- Create `kernel/test/test_wmdev.c`: broker unit tests.
- Modify `kernel/test/ktest.h`, `kernel/test/ktest.c`, `kernel/tests.mk`, and `tools/test_intent_manifest.py`: register WM tests.
- Create `user/runtime/drwin.h` and `user/runtime/drwin.c`: public app API.
- Create `user/runtime/drwin_gfx.h` and `user/runtime/drwin_gfx.c`: shared pixel/text helpers for client apps.
- Modify `user/Makefile` and `kernel/arch/arm64/arch.mk`: link `drwin.o` into user runtime objects.
- Create `user/apps/terminal.c`: terminal client app that owns its `drwin` window and pty-backed shell session.
- Create `user/apps/files.c`: files client app that lists `/`.
- Create `user/apps/processes.c`: processes client app that lists `/proc`.
- Create `user/apps/help.c`: help client app that renders static desktop help.
- Modify `user/programs.mk`: include `terminal`, `files`, `processes`, and `help`.
- Modify `user/apps/desktop.c`: register as WM server, read broker messages, composite client surfaces, send close/key/mouse/focus events, launch client app processes from the taskbar, and remove built-in app windows.
- Modify `tools/test_user_desktop_window_framework.py`: require client-window paths and reject built-in app render paths.

## Protocol Constants

Use these wire-level constants in `shared/wm_api.h`:

```c
#define DRWIN_MAX_TITLE 64
#define DRWIN_MAX_WIDTH 1024
#define DRWIN_MAX_HEIGHT 768
#define DRWIN_BPP 32
#define DRWIN_SERVER_MAGIC 0x44574d53u

typedef enum {
	DRWIN_REQ_REGISTER_SERVER = 1,
	DRWIN_REQ_CREATE_WINDOW = 2,
	DRWIN_REQ_DESTROY_WINDOW = 3,
	DRWIN_REQ_PRESENT_WINDOW = 4,
	DRWIN_REQ_SET_TITLE = 5,
	DRWIN_REQ_SHOW_WINDOW = 6,
	DRWIN_REQ_SEND_EVENT = 7,
} drwin_request_type_t;

typedef enum {
	DRWIN_MSG_CREATE_WINDOW = 1,
	DRWIN_MSG_DESTROY_WINDOW = 2,
	DRWIN_MSG_PRESENT_WINDOW = 3,
	DRWIN_MSG_SET_TITLE = 4,
	DRWIN_MSG_SHOW_WINDOW = 5,
	DRWIN_MSG_SERVER_DISCONNECT = 6,
} drwin_server_msg_type_t;
```

Use fixed-size request/message structs with explicit `uint32_t size` and `uint32_t type` fields. Every handler must reject records where `size` does not match the expected struct size.

Define concrete request structs in the same header before any code references them:

```c
typedef struct {
	uint32_t size;
	uint32_t type;
	uint32_t magic;
} drwin_register_server_request_t;

typedef struct {
	uint32_t size;
	uint32_t type;
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;
	char title[DRWIN_MAX_TITLE];
} drwin_create_window_request_t;

typedef struct {
	uint32_t size;
	uint32_t type;
	uint32_t window;
	drwin_rect_t rect;
} drwin_window_rect_request_t;

typedef struct {
	uint32_t size;
	uint32_t type;
	uint32_t window;
	uint32_t visible;
} drwin_show_window_request_t;

typedef struct {
	uint32_t size;
	uint32_t type;
	uint32_t window;
	char title[DRWIN_MAX_TITLE];
} drwin_set_title_request_t;

typedef struct {
	uint32_t size;
	uint32_t type;
	drwin_event_t event;
} drwin_send_event_request_t;

typedef struct {
	uint32_t size;
	uint32_t type;
	uint32_t window;
	drwin_rect_t rect;
	drwin_surface_info_t surface;
	char title[DRWIN_MAX_TITLE];
} drwin_server_msg_t;
```

### Task 1: Add Broker Tests First

**Files:**
- Create: `shared/wm_api.h`
- Create: `kernel/drivers/wmdev.h`
- Create: `kernel/test/test_wmdev.c`
- Modify: `kernel/test/ktest.h`
- Modify: `kernel/test/ktest.c`
- Modify: `kernel/tests.mk`
- Modify: `tools/test_intent_manifest.py`

- [ ] **Step 1: Add the shared ABI skeleton**

Create `shared/wm_api.h` with the protocol constants from the "Protocol Constants" section and these types:

```c
typedef int32_t drwin_window_t;

typedef struct {
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;
} drwin_rect_t;

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	uint32_t map_offset;
} drwin_surface_info_t;

typedef enum {
	DRWIN_EVENT_NONE = 0,
	DRWIN_EVENT_CLOSE = 1,
	DRWIN_EVENT_KEY = 2,
	DRWIN_EVENT_MOUSE = 3,
	DRWIN_EVENT_FOCUS = 4,
	DRWIN_EVENT_RESIZE = 5,
	DRWIN_EVENT_DISCONNECT = 6,
} drwin_event_type_t;

typedef struct {
	uint32_t type;
	int32_t window;
	int32_t x;
	int32_t y;
	int32_t code;
	int32_t value;
} drwin_event_t;
```

- [ ] **Step 2: Add the kernel broker test-facing header**

Create `kernel/drivers/wmdev.h` with declarations that tests can compile against:

```c
#ifndef WMDEV_H
#define WMDEV_H

#include "wm_api.h"
#include <stdint.h>

#define WMDEV_MAX_CONNECTIONS 16u
#define WMDEV_MAX_WINDOWS 16u
#define WMDEV_EVENT_QUEUE_CAP 32u

void wmdev_reset_for_test(void);
int wmdev_init(void);
int wmdev_open(uint32_t pid);
void wmdev_close(uint32_t conn_id);
int wmdev_register_server(uint32_t conn_id, uint32_t magic);
int wmdev_create_window(uint32_t conn_id,
                        const char *title,
                        int x,
                        int y,
                        int w,
                        int h,
                        uint32_t *window_out,
                        drwin_surface_info_t *surface_out);
int wmdev_present_window(uint32_t conn_id, uint32_t window, drwin_rect_t dirty);
int wmdev_destroy_window(uint32_t conn_id, uint32_t window);
int wmdev_queue_event(uint32_t window, const drwin_event_t *event);
int wmdev_read_event(uint32_t conn_id, drwin_event_t *event_out);
int wmdev_read_server_msg(uint32_t conn_id, drwin_server_msg_t *msg_out);
int wmdev_event_available(uint32_t conn_id);
int wmdev_server_msg_available(uint32_t conn_id);
int wmdev_window_owner_for_test(uint32_t window);
uint32_t wmdev_window_page_count_for_test(uint32_t window);

#endif
```

- [ ] **Step 3: Write failing broker tests**

Create `kernel/test/test_wmdev.c` with these tests:

```c
#include "ktest.h"
#include "wmdev.h"

static void test_wmdev_registers_single_server(ktest_case_t *tc)
{
	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int second = wmdev_open(11);
	KTEST_ASSERT_GE(tc, (uint32_t)server, 0u);
	KTEST_ASSERT_GE(tc, (uint32_t)second, 0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)second,
	                                                DRWIN_SERVER_MAGIC),
	                (uint32_t)-1);
}

static void test_wmdev_create_window_tracks_owner_and_surface(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int client = wmdev_open(42);
	KTEST_ASSERT_GE(tc, (uint32_t)server, 0u);
	KTEST_ASSERT_GE(tc, (uint32_t)client, 0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)client,
	                                              "paint",
	                                              20,
	                                              30,
	                                              64,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	KTEST_EXPECT_NE(tc, window, 0u);
	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_window_owner_for_test(window), 42u);
	KTEST_EXPECT_EQ(tc, surface.width, 64u);
	KTEST_EXPECT_EQ(tc, surface.height, 32u);
	KTEST_EXPECT_EQ(tc, surface.pitch, 64u * 4u);
	KTEST_EXPECT_EQ(tc, surface.bpp, 32u);
	KTEST_EXPECT_GE(tc, wmdev_window_page_count_for_test(window), 2u);
}

static void test_wmdev_rejects_cross_owner_present(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;
	drwin_rect_t dirty = {0, 0, 10, 10};

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int owner = wmdev_open(42);
	int other = wmdev_open(43);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)owner,
	                                              "owned",
	                                              0,
	                                              0,
	                                              32,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)other,
	                                               window,
	                                               dirty),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)owner,
	                                               window,
	                                               dirty),
	                0u);
}

static void test_wmdev_event_queue_round_trips_to_owner(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;
	drwin_event_t in = {DRWIN_EVENT_CLOSE, 0, 0, 0, 0, 0};
	drwin_event_t out;

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int client = wmdev_open(42);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)client,
	                                              "events",
	                                              0,
	                                              0,
	                                              32,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	in.window = (int32_t)window;

	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_queue_event(window, &in), 0u);
	KTEST_EXPECT_TRUE(tc, wmdev_event_available((uint32_t)client));
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_read_event((uint32_t)client, &out),
	                0u);
	KTEST_EXPECT_EQ(tc, out.type, DRWIN_EVENT_CLOSE);
	KTEST_EXPECT_EQ(tc, (uint32_t)out.window, window);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_wmdev_registers_single_server),
    KTEST_CASE(test_wmdev_create_window_tracks_owner_and_surface),
    KTEST_CASE(test_wmdev_rejects_cross_owner_present),
    KTEST_CASE(test_wmdev_event_queue_round_trips_to_owner),
};

static ktest_suite_t suite = KTEST_SUITE("wmdev", cases);

ktest_suite_t *ktest_suite_wmdev(void)
{
	return &suite;
}
```

- [ ] **Step 4: Register the failing test suite**

Add `ktest_suite_t *ktest_suite_wmdev(void);` to `kernel/test/ktest.h`. Add `extern ktest_suite_t *ktest_suite_wmdev(void);` to `kernel/test/ktest.c` and call `run_and_tally(ktest_suite_wmdev(), &total_pass, &total_fail);` after `ktest_suite_desktop_window()`.

Add `kernel/test/test_wmdev.o` to `KTEST_SHARED_OBJS` in `kernel/tests.mk`.

Add the four test names to a new `WMDEV_KTESTS` tuple in `tools/test_intent_manifest.py` and include a source marker for `kernel/test/test_wmdev.c` under the desktop graphical intent.

- [ ] **Step 5: Run tests to verify RED**

Run:

```bash
make ARCH=x86 KTEST=1 kernel.elf
```

Expected: compile fails because `kernel/drivers/wmdev.c` does not exist and the `wmdev_*` symbols are undefined.

- [ ] **Step 6: Commit the failing tests**

```bash
git add shared/wm_api.h kernel/drivers/wmdev.h kernel/test/test_wmdev.c kernel/test/ktest.h kernel/test/ktest.c kernel/tests.mk tools/test_intent_manifest.py
git commit -m "test: require window broker model"
```

### Task 2: Implement The WM Broker Core

**Files:**
- Create: `kernel/drivers/wmdev.c`
- Modify: `kernel/objects.mk`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/arch/x86/start_kernel.c`
- Test: `kernel/test/test_wmdev.c`

- [ ] **Step 1: Add the broker object to both builds**

Add `kernel/drivers/wmdev.o` after `kernel/drivers/inputdev.o` in `kernel/objects.mk`.

Add `kernel/drivers/wmdev.arm64.o` after `kernel/drivers/tty.arm64.o` in `ARM_SHARED_KOBJS` in `kernel/arch/arm64/arch.mk`.

- [ ] **Step 2: Implement minimal broker storage**

Create `kernel/drivers/wmdev.c` with fixed tables:

```c
#include "wmdev.h"
#include "kheap.h"
#include "kstring.h"
#include "pmm.h"
#include "sched.h"

#define PAGE_SIZE 0x1000u

typedef struct {
	uint32_t in_use;
	uint32_t pid;
	uint32_t is_server;
	drwin_event_t events[WMDEV_EVENT_QUEUE_CAP];
	uint32_t event_head;
	uint32_t event_tail;
	uint32_t event_count;
} wmdev_conn_t;

typedef struct {
	uint32_t in_use;
	uint32_t owner_pid;
	uint32_t owner_conn;
	uint32_t id;
	drwin_rect_t rect;
	char title[DRWIN_MAX_TITLE];
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t page_count;
	uint32_t pages[(DRWIN_MAX_WIDTH * DRWIN_MAX_HEIGHT * 4u + PAGE_SIZE - 1u) /
	               PAGE_SIZE];
} wmdev_window_t;

static wmdev_conn_t g_conns[WMDEV_MAX_CONNECTIONS];
static wmdev_window_t g_windows[WMDEV_MAX_WINDOWS];
static drwin_server_msg_t g_server_msgs[WMDEV_EVENT_QUEUE_CAP];
static uint32_t g_server_head;
static uint32_t g_server_tail;
static uint32_t g_server_count;
static uint32_t g_server_conn;
static uint32_t g_next_window_id = 1;
```

- [ ] **Step 3: Implement allocation, reset, and server registration**

Implement `wmdev_reset_for_test()`, `wmdev_init()`, `wmdev_open()`, `wmdev_close()`, and `wmdev_register_server()` so Task 1's first test passes. Reset must free all allocated window pages with `pmm_free_page()` before clearing tables.

- [ ] **Step 4: Run the first test and verify GREEN for server registration**

Run:

```bash
make ARCH=x86 KTEST=1 kernel.elf
```

Expected: build advances past the first `wmdev_*` undefined-symbol failures; remaining failures are missing or incomplete window creation/event functions.

- [ ] **Step 5: Implement window creation and surface allocation**

Implement `wmdev_create_window()` with these rules:

- fail if no server is registered
- fail if `conn_id` is invalid or belongs to the server
- fail if width or height is zero
- fail if width exceeds `DRWIN_MAX_WIDTH` or height exceeds `DRWIN_MAX_HEIGHT`
- pitch is `width * 4`
- page count is rounded up from `pitch * height`
- allocate one physical page per surface page with `pmm_alloc_page()`
- zero each page through `arch_page_temp_map()` and `k_memset()`
- return `map_offset = window_id << 12`
- enqueue a `DRWIN_MSG_CREATE_WINDOW` server message

- [ ] **Step 6: Implement present, destroy, and event queues**

Implement:

```c
int wmdev_present_window(uint32_t conn_id, uint32_t window, drwin_rect_t dirty);
int wmdev_destroy_window(uint32_t conn_id, uint32_t window);
int wmdev_queue_event(uint32_t window, const drwin_event_t *event);
int wmdev_read_event(uint32_t conn_id, drwin_event_t *event_out);
int wmdev_read_server_msg(uint32_t conn_id, drwin_server_msg_t *msg_out);
int wmdev_event_available(uint32_t conn_id);
int wmdev_server_msg_available(uint32_t conn_id);
```

Each client operation must confirm `g_windows[i].owner_conn == conn_id`. `wmdev_present_window()` enqueues `DRWIN_MSG_PRESENT_WINDOW` with the dirty rect. `wmdev_destroy_window()` frees pages, clears the slot, and enqueues `DRWIN_MSG_DESTROY_WINDOW`. Event reads pop FIFO order from the owning connection queue.

- [ ] **Step 7: Run tests to verify GREEN**

Run:

```bash
make ARCH=x86 KTEST=1 kernel.elf
python3 tools/check_test_intent_coverage.py
```

Expected: both commands pass.

- [ ] **Step 8: Commit broker core**

```bash
git add kernel/drivers/wmdev.c kernel/objects.mk kernel/arch/arm64/arch.mk kernel/arch/x86/start_kernel.c
git commit -m "feat: add window broker core"
```

### Task 3: Wire `/dev/wm` Into Open, Read, Write, Poll, Close, And Mmap

**Files:**
- Modify: `kernel/fs/vfs/core.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/syscall/vfs/open.c`
- Modify: `kernel/proc/syscall/helpers.c`
- Modify: `kernel/proc/resources.c`
- Modify: `kernel/proc/syscall/fd.c`
- Modify: `kernel/proc/syscall/fd_control.c`
- Modify: `kernel/proc/syscall/mem.c`
- Modify: `kernel/drivers/wmdev.h`
- Modify: `kernel/drivers/wmdev.c`
- Test: `kernel/test/test_wmdev.c`

- [ ] **Step 1: Write failing fd/mmap tests**

Add tests to `kernel/test/test_wmdev.c` that call new helpers:

```c
static void test_wmdev_mmap_page_returns_surface_pages(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;
	uint32_t phys0 = 0;
	uint32_t phys1 = 0;

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int client = wmdev_open(42);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)client,
	                                              "map",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_mmap_page((uint32_t)client,
	                                          surface.map_offset,
	                                          0,
	                                          &phys0),
	                0u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_mmap_page((uint32_t)client,
	                                          surface.map_offset,
	                                          1,
	                                          &phys1),
	                0u);
	KTEST_EXPECT_NE(tc, phys0, 0u);
	KTEST_EXPECT_NE(tc, phys1, 0u);
	KTEST_EXPECT_NE(tc, phys0, phys1);
}
```

Declare `int wmdev_mmap_page(uint32_t conn_id, uint32_t map_offset, uint32_t page_index, uint32_t *phys_out);` in `kernel/drivers/wmdev.h`.

- [ ] **Step 2: Run tests to verify RED**

Run:

```bash
make ARCH=x86 KTEST=1 kernel.elf
```

Expected: compile fails because `wmdev_mmap_page()` is declared but not defined.

- [ ] **Step 3: Add fd type and devfs entry**

In `kernel/proc/process.h`, append:

```c
FD_TYPE_WM = 12, /* window-manager broker connection */
```

Add this union member to `file_handle_t`:

```c
struct {
	uint32_t conn_id;
} wm;
```

In `kernel/fs/vfs/core.c`, add the devfs static entry:

```c
{"wm", VFS_NODE_CHARDEV, 0, "wm"},
```

- [ ] **Step 4: Special-case `/dev/wm` open and close**

In `kernel/proc/syscall/vfs/open.c`, inside `VFS_NODE_CHARDEV`, before the generic chardev assignment:

```c
if (k_strcmp(node->dev_name, "wm") == 0) {
	int conn = wmdev_open(proc->pid);
	if (conn < 0) {
		proc_fd_entries(proc)[fd].type = FD_TYPE_NONE;
		return -1;
	}
	proc_fd_entries(proc)[fd].type = FD_TYPE_WM;
	proc_fd_entries(proc)[fd].u.wm.conn_id = (uint32_t)conn;
	proc_fd_entries(proc)[fd].writable = 1;
	return fd;
}
```

Include `wmdev.h` and `kstring.h` if not already available.

In both `fd_close_one()` and `proc_fd_table_close_all()`, add:

```c
if (fh->type == FD_TYPE_WM)
	wmdev_close(fh->u.wm.conn_id);
```

- [ ] **Step 5: Route read and write**

Add `wmdev_read_user_record()` and `wmdev_write_user_record()` declarations:

```c
int wmdev_read_user_record(uint32_t conn_id, uint8_t *buf, uint32_t count);
int wmdev_write_user_record(uint32_t conn_id, const uint8_t *buf, uint32_t count);
```

In `syscall_read_fd()`, before pipe/file handling:

```c
if (fh->type == FD_TYPE_WM) {
	uint8_t kbuf[USER_IO_CHUNK];
	int n;

	if (count > USER_IO_CHUNK)
		count = USER_IO_CHUNK;
	if (uaccess_prepare(cur, user_buf, count, 1) != 0)
		return (uint32_t)-1;
	n = wmdev_read_user_record(fh->u.wm.conn_id, kbuf, count);
	if (n <= 0)
		return (uint32_t)n;
	if (uaccess_copy_to_user(cur, user_buf, kbuf, (uint32_t)n) != 0)
		return (uint32_t)-1;
	return (uint32_t)n;
}
```

In `syscall_write_fd()`, before pipe/file handling:

```c
if (fh->type == FD_TYPE_WM) {
	uint8_t kbuf[USER_IO_CHUNK];

	if (count > USER_IO_CHUNK)
		return (uint32_t)-1;
	if (uaccess_prepare(cur, user_buf, count, 0) != 0)
		return (uint32_t)-1;
	if (uaccess_copy_from_user(cur, kbuf, user_buf, count) != 0)
		return (uint32_t)-1;
	return (uint32_t)wmdev_write_user_record(fh->u.wm.conn_id, kbuf, count);
}
```

- [ ] **Step 6: Route poll**

In `linux_poll_revents()` in `kernel/proc/syscall/fd_control.c`, add:

```c
else if (fh->type == FD_TYPE_WM) {
	if (wmdev_event_available(fh->u.wm.conn_id) ||
	    wmdev_server_msg_available(fh->u.wm.conn_id))
		rev |= LINUX_POLLIN;
}
```

For output readiness, add `fh->type == FD_TYPE_WM` to the `LINUX_POLLOUT` branch.

- [ ] **Step 7: Route non-contiguous mmap**

In `kernel/proc/syscall/mem.c`, add a helper `syscall_mmap_wmdev()` modeled on `syscall_mmap_chardev()` but mapping one page at a time:

```c
static int syscall_mmap_wmdev(process_t *cur,
                              uint32_t hint,
                              uint32_t length,
                              uint32_t prot,
                              uint32_t fd,
                              uint32_t file_offset,
                              uint32_t *addr_out)
{
	file_handle_t *fh;
	uint32_t map_len;
	uint32_t map_addr = 0;
	uint32_t vma_flags;
	uint32_t map_flags;
	arch_aspace_t aspace;

	if (!cur || !addr_out || fd >= MAX_FDS || length == 0)
		return -1;
	if (file_offset & (PAGE_SIZE - 1u))
		return -1;
	fh = &proc_fd_entries(cur)[fd];
	if (fh->type != FD_TYPE_WM)
		return -1;
	map_len = (length + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
	if (map_len == 0)
		return -1;
	vma_flags = prot_to_vma_flags(prot) & ~(uint32_t)VMA_FLAG_ANON;
	if (vma_map_anonymous(cur, hint, map_len, vma_flags, &map_addr) != 0)
		return -1;
	aspace = (arch_aspace_t)cur->pd_phys;
	map_flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_IO;
	if (prot_has_user_access(prot))
		map_flags |= ARCH_MM_MAP_USER;
	if (prot & LINUX_PROT_WRITE)
		map_flags |= ARCH_MM_MAP_WRITE;
	for (uint32_t off = 0; off < map_len; off += PAGE_SIZE) {
		uint32_t phys = 0;
		if (wmdev_mmap_page(fh->u.wm.conn_id,
		                    file_offset,
		                    off / PAGE_SIZE,
		                    &phys) != 0)
			goto fail;
		if (arch_mm_map(aspace, map_addr + off, phys, map_flags) != 0)
			goto fail;
	}
	*addr_out = map_addr;
	return 0;
fail:
	syscall_unmap_user_range(cur, map_addr, map_addr + map_len);
	(void)vma_unmap_range(cur, map_addr, map_addr + map_len);
	return -1;
}
```

Call it from both `syscall_case_mmap()` and `syscall_case_mmap2()` when the fd type is `FD_TYPE_WM`; require `MAP_SHARED`.

- [ ] **Step 8: Implement user-record read/write dispatch in `wmdev.c`**

Decode client records by `type` and `size`. A client connection may send create, destroy, present, set-title, and show requests. The server connection may send register-server and send-event requests. `wmdev_read_user_record()` returns server messages to the server connection and `drwin_event_t` records to client connections.

- [ ] **Step 9: Run tests to verify GREEN**

Run:

```bash
make ARCH=x86 KTEST=1 kernel.elf
python3 tools/check_test_intent_coverage.py
```

Expected: both commands pass.

- [ ] **Step 10: Commit fd and mmap wiring**

```bash
git add kernel/fs/vfs/core.c kernel/proc/process.h kernel/proc/syscall/vfs/open.c kernel/proc/syscall/helpers.c kernel/proc/resources.c kernel/proc/syscall/fd.c kernel/proc/syscall/fd_control.c kernel/proc/syscall/mem.c kernel/drivers/wmdev.h kernel/drivers/wmdev.c kernel/test/test_wmdev.c tools/test_intent_manifest.py
git commit -m "feat: wire window broker device"
```

### Task 4: Add The User Runtime API And Drawing Helpers

**Files:**
- Create: `user/runtime/drwin.h`
- Create: `user/runtime/drwin.c`
- Create: `user/runtime/drwin_gfx.h`
- Create: `user/runtime/drwin_gfx.c`
- Modify: `user/Makefile`
- Modify: `kernel/arch/arm64/arch.mk`
- Test: `tools/test_user_window_api.py`

- [ ] **Step 1: Write a failing runtime policy test**

Create `tools/test_user_window_api.py`:

```python
#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    header = ROOT / "user" / "runtime" / "drwin.h"
    source = ROOT / "user" / "runtime" / "drwin.c"
    gfx_header = ROOT / "user" / "runtime" / "drwin_gfx.h"
    gfx_source = ROOT / "user" / "runtime" / "drwin_gfx.c"
    makefile = (ROOT / "user" / "Makefile").read_text()
    arm64 = (ROOT / "kernel" / "arch" / "arm64" / "arch.mk").read_text()
    failures = []
    if not header.exists():
        failures.append("user/runtime/drwin.h is required")
    if not source.exists():
        failures.append("user/runtime/drwin.c is required")
    if not gfx_header.exists():
        failures.append("user/runtime/drwin_gfx.h is required")
    if not gfx_source.exists():
        failures.append("user/runtime/drwin_gfx.c is required")
    if "drwin.o" not in makefile:
        failures.append("x86 user runtime must link drwin.o")
    if "drwin_gfx.o" not in makefile:
        failures.append("x86 user runtime must link drwin_gfx.o")
    if "drwin.o" not in arm64:
        failures.append("arm64 user runtime must link drwin.o")
    if "drwin_gfx.o" not in arm64:
        failures.append("arm64 user runtime must link drwin_gfx.o")
    if failures:
        print("user window API is incomplete:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("user window API runtime is wired")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Run the test to verify RED**

Run:

```bash
python3 tools/test_user_window_api.py
```

Expected: FAIL with missing `drwin.h`, missing `drwin.c`, missing drawing helper files, and missing runtime object wiring.

- [ ] **Step 3: Add `drwin.h`**

Create `user/runtime/drwin.h`:

```c
#ifndef DRWIN_H
#define DRWIN_H

#include "wm_api.h"

typedef struct {
	int width;
	int height;
	int pitch;
	int bpp;
	void *pixels;
} drwin_surface_t;

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
int drwin_fd_for_poll(void);

#endif
```

- [ ] **Step 4: Add `drwin.c`**

Implement `drwin.c` with a single static fd:

```c
#include "drwin.h"
#include "mman.h"
#include "string.h"
#include "syscall.h"

static int g_drwin_fd = -1;
static drwin_surface_info_t g_surfaces[16];

int drwin_connect(void)
{
	if (g_drwin_fd >= 0)
		return 0;
	g_drwin_fd = sys_open_flags("/dev/wm", SYS_O_RDWR, 0);
	return g_drwin_fd >= 0 ? 0 : -1;
}

int drwin_fd_for_poll(void)
{
	return g_drwin_fd;
}
```

Add wrappers that build fixed request structs from `shared/wm_api.h`, call `sys_fwrite(g_drwin_fd, ...)`, read the create response, and call `mmap(0, surface.pitch * surface.height, PROT_READ | PROT_WRITE, MAP_SHARED, g_drwin_fd, surface.map_offset)` in `drwin_map_surface()`.

- [ ] **Step 5: Add shared drawing helpers**

Create `user/runtime/drwin_gfx.h`:

```c
#ifndef DRWIN_GFX_H
#define DRWIN_GFX_H

#include "drwin.h"
#include "stdint.h"

#define DRWIN_GLYPH_W 8
#define DRWIN_GLYPH_H 16

void drwin_fill_rect(drwin_surface_t *surface,
                     int x,
                     int y,
                     int w,
                     int h,
                     uint32_t color);
void drwin_draw_text(drwin_surface_t *surface,
                     int x,
                     int y,
                     const char *text,
                     uint32_t fg,
                     uint32_t bg);

#endif
```

Create `user/runtime/drwin_gfx.c` by moving the font-glyph dependency out of `desktop.c` and into the runtime drawing layer. It should include `desktop_font.h` so all client apps use the same bitmap font:

```c
#include "drwin_gfx.h"
#include "desktop_font.h"

void drwin_fill_rect(drwin_surface_t *surface,
                     int x,
                     int y,
                     int w,
                     int h,
                     uint32_t color)
{
	if (!surface || !surface->pixels || w <= 0 || h <= 0)
		return;
	for (int yy = y; yy < y + h; yy++) {
		if (yy < 0 || yy >= surface->height)
			continue;
		uint32_t *row = (uint32_t *)((char *)surface->pixels +
		                             yy * surface->pitch);
		for (int xx = x; xx < x + w; xx++) {
			if (xx >= 0 && xx < surface->width)
				row[xx] = color;
		}
	}
}
```

`drwin_draw_text()` should use `desktop_font_glyph()` and `drwin_fill_rect()`-style clipping, so Terminal, Files, Processes, and Help do not copy private desktop text rendering code.

- [ ] **Step 6: Wire runtime objects into both user builds**

Add `$(RUNTIME_OBJ_DIR)/drwin.o` and `$(RUNTIME_OBJ_DIR)/drwin_gfx.o` to `C_RUNTIME_OBJS` in `user/Makefile`.

Add `$(ARM_USER_RUNTIME_OBJ_DIR)/drwin.o` and `$(ARM_USER_RUNTIME_OBJ_DIR)/drwin_gfx.o` to `ARM_USER_C_RUNTIME_OBJS` in `kernel/arch/arm64/arch.mk`.

Add shared and app include paths to runtime compile rules if `wm_api.h` or `desktop_font.h` is not found.

- [ ] **Step 7: Run tests to verify GREEN**

Run:

```bash
python3 tools/test_user_window_api.py
make -C user print-progs
make -C user BUILD_MODE=debug ../build/user/x86/runtime/libc.a
```

Expected: all commands pass.

- [ ] **Step 8: Commit runtime API**

```bash
git add tools/test_user_window_api.py user/runtime/drwin.h user/runtime/drwin.c user/runtime/drwin_gfx.h user/runtime/drwin_gfx.c user/Makefile kernel/arch/arm64/arch.mk
git commit -m "feat: add user window runtime"
```

### Task 5: Convert Desktop Apps To Client Processes

**Files:**
- Create: `user/apps/terminal.c`
- Create: `user/apps/files.c`
- Create: `user/apps/processes.c`
- Create: `user/apps/help.c`
- Modify: `user/programs.mk`
- Test: `tools/test_user_window_api.py`

- [ ] **Step 1: Extend the policy test**

Add to `tools/test_user_window_api.py`:

```python
    programs = (ROOT / "user" / "programs.mk").read_text()
    for app_name in ("terminal", "files", "processes", "help"):
        app = ROOT / "user" / "apps" / f"{app_name}.c"
        if not app.exists():
            failures.append(f"user/apps/{app_name}.c client app is required")
        if app_name not in programs:
            failures.append(f"user/programs.mk must include {app_name}")
        if app.exists() and "drwin_create_window" not in app.read_text():
            failures.append(f"{app_name} must create a drwin window")
```

- [ ] **Step 2: Run the test to verify RED**

Run:

```bash
python3 tools/test_user_window_api.py
```

Expected: FAIL with missing Terminal, Files, Processes, Help app files and program manifest wiring.

- [ ] **Step 3: Create shared app rendering pattern**

Each app should use this pattern:

```c
#include "drwin.h"
#include "drwin_gfx.h"
#include "syscall.h"

static int open_window(const char *title,
                       int w,
                       int h,
                       drwin_window_t *win,
                       drwin_surface_t *surface)
{
	if (drwin_connect() != 0)
		return -1;
	if (drwin_create_window(title, 80, 80, w, h, win) != 0)
		return -1;
	return drwin_map_surface(*win, surface);
}
```

- [ ] **Step 4: Create `files.c`, `processes.c`, and `help.c`**

`files.c` lists `/`, `processes.c` lists `/proc`, and `help.c` draws static help text. The files app body should look like:

```c
static void draw_directory(drwin_surface_t *surface,
                           const char *path,
                           const char *heading)
{
	char dents[512];
	int n = sys_getdents(path, dents, (int)sizeof(dents));
	int y = 12;

	drwin_fill_rect(surface, 0, 0, surface->width, surface->height, 0x00141414u);
	drwin_draw_text(surface, 12, y, heading, 0x00d0d0d0u, 0x00141414u);
	y += DRWIN_GLYPH_H;
	for (int i = 0; i < n && y + DRWIN_GLYPH_H < surface->height;) {
		const char *entry = dents + i;
		int len = 0;
		while (i + len < n && entry[len])
			len++;
		if (i + len >= n)
			break;
		drwin_draw_text(surface, 12, y, entry, 0x00d0d0d0u, 0x00141414u);
		y += DRWIN_GLYPH_H;
		i += len + 1;
	}
}
```

`help.c` should draw the same user-facing help currently rendered by the desktop Help window, using `drwin_draw_text()`.

- [ ] **Step 5: Create `terminal.c`**

Move the terminal emulator state and pty session ownership out of `desktop.c` into `user/apps/terminal.c`. The terminal app should:

- create a `drwin` window titled `Terminal`
- map its pixel surface
- open `/dev/ptmx` and `/dev/pts0`
- fork/exec `/bin/shell` with the pty slave as stdin/stdout/stderr
- poll both the pty master and `/dev/wm`
- render terminal bytes into its own mapped surface
- forward `DRWIN_EVENT_KEY` values into the pty master
- exit cleanly on `DRWIN_EVENT_CLOSE`

Use this event loop shape:

```c
for (;;) {
	sys_pollfd_t pfds[2];
	pfds[0].fd = ptmx;
	pfds[0].events = SYS_POLLIN;
	pfds[0].revents = 0;
	pfds[1].fd = drwin_fd_for_poll();
	pfds[1].events = SYS_POLLIN;
	pfds[1].revents = 0;
	if (sys_poll(pfds, 2, -1) <= 0)
		continue;
	if (pfds[0].revents & SYS_POLLIN)
		drain_pty_and_present();
	if (pfds[1].revents & SYS_POLLIN)
		drain_drwin_events_and_forward_keys();
}
```

The `drwin_fd_for_poll()` function is required because the Terminal client must poll `/dev/wm` and its pty master in one syscall.

- [ ] **Step 6: Add apps to `C_PROGS`**

Modify `user/programs.mk` so `terminal`, `files`, `processes`, and `help` are in the C program list.

- [ ] **Step 7: Run tests to verify GREEN**

Run:

```bash
python3 tools/test_user_window_api.py
make -C user terminal files processes help
```

Expected: both commands pass.

- [ ] **Step 8: Commit client apps**

```bash
git add tools/test_user_window_api.py user/apps/terminal.c user/apps/files.c user/apps/processes.c user/apps/help.c user/programs.mk user/runtime/drwin.h user/runtime/drwin.c
git commit -m "feat: convert desktop apps to window clients"
```

### Task 6: Make Desktop A Pure Window Server And Launcher

**Files:**
- Modify: `user/apps/desktop.c`
- Modify: `tools/test_user_desktop_window_framework.py`
- Test: `tools/test_user_desktop_window_framework.py`

- [ ] **Step 1: Write failing desktop policy checks**

Update `tools/test_user_desktop_window_framework.py` required helpers:

```python
required = (
    "render_window_frame(",
    "window_at_pointer(",
    "wm_server_connect(",
    "drain_wm_server_messages(",
    "render_client_window(",
    "send_client_window_event(",
    "launch_taskbar_app(",
)
forbidden = (
    "render_app_content(",
    "render_app_window(",
    "start_terminal_session(",
    "g_grid[",
    "DRUNIX_TASKBAR_APP_FILES]",
    "DRUNIX_TASKBAR_APP_PROCESSES]",
    "DRUNIX_TASKBAR_APP_HELP]",
)
```

- [ ] **Step 2: Run the test to verify RED**

Run:

```bash
python3 tools/test_user_desktop_window_framework.py
```

Expected: FAIL listing missing WM helpers and forbidden built-in app paths still present in `desktop.c`.

- [ ] **Step 3: Add desktop-side client window state**

In `user/apps/desktop.c`, include `wm_api.h` and add:

```c
#define DESKTOP_CLIENT_WINDOWS 8

typedef struct {
	int used;
	uint32_t id;
	int x;
	int y;
	int w;
	int h;
	int minimized;
	char title[DRWIN_MAX_TITLE];
	uint32_t *pixels;
	uint32_t pitch;
} desktop_client_window_t;

static int g_wm_fd = -1;
static desktop_client_window_t g_client_windows[DESKTOP_CLIENT_WINDOWS];
```

- [ ] **Step 4: Register as server**

Add `wm_server_connect()`:

```c
static int wm_server_connect(void)
{
	drwin_register_server_request_t req;
	g_wm_fd = sys_open_flags("/dev/wm", SYS_O_RDWR, 0);
	if (g_wm_fd < 0)
		return -1;
	req.size = sizeof(req);
	req.type = DRWIN_REQ_REGISTER_SERVER;
	req.magic = DRWIN_SERVER_MAGIC;
	return sys_fwrite(g_wm_fd, (const char *)&req, sizeof(req)) ==
	               (int)sizeof(req)
	           ? 0
	           : -1;
}
```

Call it after `sys_display_claim()` succeeds. If registration fails, write `desktop: wm server unavailable\n` and return `1`; without the window broker there are no app windows to show.

- [ ] **Step 5: Drain server messages**

Add `drain_wm_server_messages()` to read fixed `drwin_server_msg_t` records from `g_wm_fd`. On create, allocate a `desktop_client_window_t` slot, copy title and geometry, then map the client surface using `msg.surface.map_offset`. On present, call `present_dirty_rect()` for the window rect union with the dirty rect translated into screen coordinates. On destroy, clear the slot and repaint.

- [ ] **Step 6: Render and hit-test client windows**

Add `render_client_window()` that calls `render_window_frame()` and copies pixels from the mapped client buffer into the content area. `render_windows()` should iterate only `g_client_windows`; there should be no Terminal, Files, Processes, or Help render path in `desktop.c`.

- [ ] **Step 7: Send close, key, mouse, and focus events**

Add `send_client_window_event()`:

```c
static void send_client_window_event(uint32_t window,
                                     uint32_t type,
                                     int x,
                                     int y,
                                     int code,
                                     int value)
{
	drwin_send_event_request_t req;
	if (g_wm_fd < 0)
		return;
	req.size = sizeof(req);
	req.type = DRWIN_REQ_SEND_EVENT;
	req.event.type = type;
	req.event.window = (int32_t)window;
	req.event.x = x;
	req.event.y = y;
	req.event.code = code;
	req.event.value = value;
	(void)sys_fwrite(g_wm_fd, (const char *)&req, sizeof(req));
}
```

When a client title-bar close button is clicked, send `DRWIN_EVENT_CLOSE`. When a client window gains focus, send focus events to old and new focused clients. Keyboard input always goes to the focused client window; the desktop does not know whether that client is Terminal, Files, Processes, Help, or another app.

- [ ] **Step 8: Launch taskbar apps as processes**

Add:

```c
static int launch_taskbar_app(int app)
{
	const char *path = 0;
	char *argv[2];
	if (app == DRUNIX_TASKBAR_APP_TERMINAL)
		path = "/bin/terminal";
	else if (app == DRUNIX_TASKBAR_APP_FILES)
		path = "/bin/files";
	else if (app == DRUNIX_TASKBAR_APP_PROCESSES)
		path = "/bin/processes";
	else if (app == DRUNIX_TASKBAR_APP_HELP || app == DRUNIX_TASKBAR_APP_MENU)
		path = "/bin/help";
	if (!path)
		return -1;
	int pid = sys_fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		argv[0] = (char *)path;
		argv[1] = 0;
		sys_exec(path, argv, 1);
		sys_exit(127);
	}
	return 0;
}
```

Replace `open_or_toggle_terminal()` and `open_or_toggle_app()` with `launch_taskbar_app()`. Taskbar active state should come from `g_client_windows` titles or window ids, not `g_app_open[]`.

- [ ] **Step 9: Poll WM fd in the desktop loop**

Add `g_wm_fd` to the `sys_pollfd_t pfds[]` set in `main()`. When readable, call `drain_wm_server_messages()`.

- [ ] **Step 10: Remove built-in window state**

Delete terminal and built-in app state from `desktop.c`, including `g_grid`, cursor state, terminal pty/session helpers, `g_app_open`, `g_app_minimized`, `g_app_x`, `g_app_y`, `render_terminal()`, `render_app_content()`, `render_app_window()`, `open_or_toggle_terminal()`, and `open_or_toggle_app()`.

- [ ] **Step 11: Run tests to verify GREEN**

Run:

```bash
python3 tools/test_user_desktop_window_framework.py
make -C user desktop
```

Expected: both commands pass and the policy test reports no forbidden built-in app paths.

- [ ] **Step 12: Commit desktop integration**

```bash
git add user/apps/desktop.c tools/test_user_desktop_window_framework.py
git commit -m "feat: make desktop a window server"
```

### Task 7: Final Validation And Documentation Notes

**Files:**
- Modify: `docs/superpowers/specs/2026-04-27-userland-window-api-design.md` if implementation diverged from the approved spec
- Modify: `docs/ch28-desktop.md` only if the implementation changes user-visible desktop behavior enough to keep the chapter accurate

- [ ] **Step 1: Run fast policy checks**

Run:

```bash
python3 tools/test_user_window_api.py
python3 tools/test_user_desktop_window_framework.py
python3 tools/check_userland_runtime_lanes.py
python3 tools/check_test_intent_coverage.py
make -C user print-progs
```

Expected: all pass.

- [ ] **Step 2: Run build checks**

Run:

```bash
make ARCH=x86 KTEST=1 kernel.elf
make -C user desktop terminal files processes help
```

Expected: all pass.

- [ ] **Step 3: Run boot-level validation**

Run:

```bash
make ARCH=x86 test-headless
```

Expected: KTEST summary reports `fail=0`.

- [ ] **Step 4: Manually smoke the desktop**

Run:

```bash
make ARCH=x86 run
```

In the desktop, click the taskbar buttons for Terminal, Files, Processes, and Help. Expected: each app appears as a client-owned window, can be focused, and closes when its title-bar close button is clicked. From the Terminal client, run `echo window-api` and confirm shell output appears inside the Terminal window.

- [ ] **Step 5: Commit validation/doc cleanup**

If docs changed:

```bash
git add docs/ch28-desktop.md docs/superpowers/specs/2026-04-27-userland-window-api-design.md
git commit -m "docs: update desktop window API notes"
```

If no docs changed, do not create an empty commit.

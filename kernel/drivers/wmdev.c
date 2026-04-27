/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "wmdev.h"
#include "arch.h"
#include "kheap.h"
#include "kstring.h"
#include "pmm.h"
#include "sched.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000u
#endif

#define WMDEV_CLIENT_RECORD_MAX                                                \
	((uint32_t)sizeof(drwin_create_window_response_t))
#define WMDEV_SURFACE_MAP_STRIDE                                               \
	((((DRWIN_MAX_WIDTH * DRWIN_MAX_HEIGHT * 4u) + PAGE_SIZE - 1u) /          \
	  PAGE_SIZE) *                                                            \
	 PAGE_SIZE)

typedef struct {
	uint32_t in_use;
	uint32_t refs;
	uint32_t pid;
	uint32_t is_server;
	drwin_event_t events[WMDEV_EVENT_QUEUE_CAP];
	uint32_t event_head;
	uint32_t event_tail;
	uint32_t event_count;
	uint8_t records[WMDEV_EVENT_QUEUE_CAP][WMDEV_CLIENT_RECORD_MAX];
	uint32_t record_sizes[WMDEV_EVENT_QUEUE_CAP];
	uint32_t record_head;
	uint32_t record_tail;
	uint32_t record_count;
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

static int conn_valid(uint32_t conn_id)
{
	return conn_id < WMDEV_MAX_CONNECTIONS && g_conns[conn_id].in_use;
}

static int server_registered(void)
{
	return conn_valid(g_server_conn) && g_conns[g_server_conn].is_server;
}

static wmdev_window_t *find_window(uint32_t window)
{
	for (uint32_t i = 0; i < WMDEV_MAX_WINDOWS; i++) {
		if (g_windows[i].in_use && g_windows[i].id == window)
			return &g_windows[i];
	}
	return 0;
}

static uint32_t map_offset_for_window(uint32_t window)
{
	return window * WMDEV_SURFACE_MAP_STRIDE;
}

static wmdev_window_t *find_window_by_map_offset(uint32_t map_offset)
{
	for (uint32_t i = 0; i < WMDEV_MAX_WINDOWS; i++) {
		if (g_windows[i].in_use &&
		    map_offset_for_window(g_windows[i].id) == map_offset)
			return &g_windows[i];
	}
	return 0;
}

static void free_window_pages(wmdev_window_t *win)
{
	if (!win)
		return;
	for (uint32_t i = 0; i < win->page_count; i++) {
		if (win->pages[i])
			pmm_free_page(win->pages[i]);
	}
	win->page_count = 0;
}

static void clear_window(wmdev_window_t *win)
{
	free_window_pages(win);
	k_memset(win, 0, sizeof(*win));
}

static int enqueue_server_msg(const drwin_server_msg_t *msg)
{
	if (!server_registered() || !msg)
		return -1;
	if (g_server_count >= WMDEV_EVENT_QUEUE_CAP)
		return -1;

	g_server_msgs[g_server_head] = *msg;
	g_server_head = (g_server_head + 1u) % WMDEV_EVENT_QUEUE_CAP;
	g_server_count++;
	return 0;
}

static uint32_t remove_server_msgs_for_window(uint32_t window)
{
	uint32_t old_tail = g_server_tail;
	uint32_t old_count = g_server_count;
	uint32_t kept = 0;
	uint32_t removed = 0;

	g_server_head = 0;
	g_server_tail = 0;
	g_server_count = 0;
	for (uint32_t i = 0; i < old_count; i++) {
		uint32_t idx = (old_tail + i) % WMDEV_EVENT_QUEUE_CAP;
		if (g_server_msgs[idx].window == window) {
			removed++;
			continue;
		}
		g_server_msgs[kept++] = g_server_msgs[idx];
	}
	for (uint32_t i = kept; i < WMDEV_EVENT_QUEUE_CAP; i++)
		k_memset(&g_server_msgs[i], 0, sizeof(g_server_msgs[i]));
	g_server_count = kept;
	g_server_head = kept % WMDEV_EVENT_QUEUE_CAP;
	return removed;
}

static void drop_server_msg_at_order(uint32_t drop_order)
{
	uint32_t old_tail = g_server_tail;
	uint32_t old_count = g_server_count;
	uint32_t kept = 0;

	g_server_head = 0;
	g_server_tail = 0;
	g_server_count = 0;
	for (uint32_t i = 0; i < old_count; i++) {
		uint32_t idx = (old_tail + i) % WMDEV_EVENT_QUEUE_CAP;
		if (i == drop_order)
			continue;
		g_server_msgs[kept++] = g_server_msgs[idx];
	}
	for (uint32_t i = kept; i < WMDEV_EVENT_QUEUE_CAP; i++)
		k_memset(&g_server_msgs[i], 0, sizeof(g_server_msgs[i]));
	g_server_count = kept;
	g_server_head = kept % WMDEV_EVENT_QUEUE_CAP;
}

static int server_msg_is_stale_update(uint32_t type)
{
	return type == DRWIN_MSG_PRESENT_WINDOW ||
	       type == DRWIN_MSG_SET_TITLE ||
	       type == DRWIN_MSG_SHOW_WINDOW;
}

static void make_room_for_authoritative_server_msg(void)
{
	if (g_server_count < WMDEV_EVENT_QUEUE_CAP)
		return;

	for (uint32_t i = 0; i < g_server_count; i++) {
		uint32_t idx = (g_server_tail + i) % WMDEV_EVENT_QUEUE_CAP;
		if (server_msg_is_stale_update(g_server_msgs[idx].type)) {
			drop_server_msg_at_order(i);
			return;
		}
	}

	/*
	 * wmdev_close() cannot fail or ask the caller to retry. When the queue
	 * contains only authoritative messages, drop the oldest one so the
	 * closing window's destroy can be delivered instead of leaking pages or
	 * leaving the server with stale queued updates.
	 */
	drop_server_msg_at_order(0);
}

static int enqueue_event_to_conn(uint32_t conn_id, const drwin_event_t *event)
{
	wmdev_conn_t *conn;

	if (!conn_valid(conn_id) || !event)
		return -1;
	conn = &g_conns[conn_id];
	if (conn->event_count >= WMDEV_EVENT_QUEUE_CAP)
		return -1;

	conn->events[conn->event_head] = *event;
	conn->event_head = (conn->event_head + 1u) % WMDEV_EVENT_QUEUE_CAP;
	conn->event_count++;
	return 0;
}

static void fill_surface(drwin_surface_info_t *surface,
                         uint32_t window,
                         uint32_t width,
                         uint32_t height,
                         uint32_t pitch);

static int enqueue_client_record(uint32_t conn_id,
                                 const void *record,
                                 uint32_t size)
{
	wmdev_conn_t *conn;

	if (!conn_valid(conn_id) || !record || size == 0 ||
	    size > WMDEV_CLIENT_RECORD_MAX)
		return -1;
	conn = &g_conns[conn_id];
	if (conn->record_count >= WMDEV_EVENT_QUEUE_CAP)
		return -1;

	k_memset(conn->records[conn->record_head],
	         0,
	         sizeof(conn->records[conn->record_head]));
	k_memcpy(conn->records[conn->record_head], record, size);
	conn->record_sizes[conn->record_head] = size;
	conn->record_head = (conn->record_head + 1u) % WMDEV_EVENT_QUEUE_CAP;
	conn->record_count++;
	return 0;
}

static int enqueue_destroy_for_close(const wmdev_window_t *win)
{
	drwin_server_msg_t msg;

	if (!win || !server_registered())
		return 0;

	remove_server_msgs_for_window(win->id);
	make_room_for_authoritative_server_msg();

	k_memset(&msg, 0, sizeof(msg));
	msg.size = sizeof(drwin_server_msg_t);
	msg.type = DRWIN_MSG_DESTROY_WINDOW;
	msg.window = win->id;
	msg.rect = win->rect;
	return enqueue_server_msg(&msg);
}

static int enqueue_surface_update(uint32_t conn_id,
                                  uint32_t window,
                                  uint32_t type,
                                  drwin_rect_t rect,
                                  const char *title,
                                  uint32_t visible)
{
	wmdev_window_t *win;
	drwin_server_msg_t msg;

	if (!conn_valid(conn_id))
		return -1;
	win = find_window(window);
	if (!win || win->owner_conn != conn_id)
		return -1;

	k_memset(&msg, 0, sizeof(msg));
	msg.size = sizeof(drwin_server_msg_t);
	msg.type = type;
	msg.window = window;
	msg.rect = rect;
	fill_surface(&msg.surface, win->id, win->width, win->height, win->pitch);
	if (type == DRWIN_MSG_SET_TITLE) {
		k_strncpy(win->title, title ? title : "", DRWIN_MAX_TITLE - 1u);
		win->title[DRWIN_MAX_TITLE - 1u] = '\0';
		k_strncpy(msg.title, win->title, DRWIN_MAX_TITLE - 1u);
		msg.title[DRWIN_MAX_TITLE - 1u] = '\0';
	} else if (type == DRWIN_MSG_SHOW_WINDOW) {
		msg.visible = visible ? 1u : 0u;
	}
	return enqueue_server_msg(&msg);
}

static void fill_surface(drwin_surface_info_t *surface,
                         uint32_t window,
                         uint32_t width,
                         uint32_t height,
                         uint32_t pitch)
{
	if (!surface)
		return;
	surface->width = width;
	surface->height = height;
	surface->pitch = pitch;
	surface->bpp = DRWIN_BPP;
	surface->map_offset = map_offset_for_window(window);
}

void wmdev_reset_for_test(void)
{
	for (uint32_t i = 0; i < WMDEV_MAX_WINDOWS; i++)
		free_window_pages(&g_windows[i]);

	k_memset(g_conns, 0, sizeof(g_conns));
	k_memset(g_windows, 0, sizeof(g_windows));
	k_memset(g_server_msgs, 0, sizeof(g_server_msgs));
	g_server_head = 0;
	g_server_tail = 0;
	g_server_count = 0;
	g_server_conn = 0;
	g_next_window_id = 1;
}

int wmdev_init(void)
{
	wmdev_reset_for_test();
	return 0;
}

int wmdev_open(uint32_t pid)
{
	for (uint32_t i = 0; i < WMDEV_MAX_CONNECTIONS; i++) {
		if (!g_conns[i].in_use) {
			k_memset(&g_conns[i], 0, sizeof(g_conns[i]));
			g_conns[i].in_use = 1;
			g_conns[i].refs = 1;
			g_conns[i].pid = pid;
			return (int)i;
		}
	}
	return -1;
}

int wmdev_retain(uint32_t conn_id)
{
	if (!conn_valid(conn_id) || g_conns[conn_id].refs == UINT32_MAX)
		return -1;
	g_conns[conn_id].refs++;
	return 0;
}

void wmdev_close(uint32_t conn_id)
{
	uint32_t notified[WMDEV_MAX_CONNECTIONS];

	if (!conn_valid(conn_id))
		return;
	if (g_conns[conn_id].refs > 1) {
		g_conns[conn_id].refs--;
		return;
	}

	if (g_conns[conn_id].is_server) {
		drwin_event_t disconnect;

		k_memset(notified, 0, sizeof(notified));
		k_memset(&disconnect, 0, sizeof(disconnect));
		disconnect.type = DRWIN_EVENT_DISCONNECT;
		for (uint32_t i = 0; i < WMDEV_MAX_WINDOWS; i++) {
			uint32_t owner;

			if (!g_windows[i].in_use)
				continue;
			owner = g_windows[i].owner_conn;
			if (owner < WMDEV_MAX_CONNECTIONS && !notified[owner]) {
				(void)enqueue_event_to_conn(owner, &disconnect);
				notified[owner] = 1;
			}
			clear_window(&g_windows[i]);
		}

		k_memset(g_server_msgs, 0, sizeof(g_server_msgs));
		g_server_head = 0;
		g_server_tail = 0;
		g_server_count = 0;
		g_server_conn = 0;
	} else {
		for (uint32_t i = 0; i < WMDEV_MAX_WINDOWS; i++) {
			if (g_windows[i].in_use && g_windows[i].owner_conn == conn_id) {
				(void)enqueue_destroy_for_close(&g_windows[i]);
				clear_window(&g_windows[i]);
			}
		}
	}

	k_memset(&g_conns[conn_id], 0, sizeof(g_conns[conn_id]));
}

int wmdev_register_server(uint32_t conn_id, uint32_t magic)
{
	if (magic != DRWIN_SERVER_MAGIC || !conn_valid(conn_id))
		return -1;
	if (server_registered())
		return -1;

	g_conns[conn_id].is_server = 1;
	g_server_conn = conn_id;
	return 0;
}

int wmdev_create_window(uint32_t conn_id,
                        const char *title,
                        int x,
                        int y,
                        int w,
                        int h,
                        uint32_t *window_out,
                        drwin_surface_info_t *surface_out)
{
	wmdev_window_t *win = 0;
	drwin_surface_info_t surface;
	drwin_server_msg_t msg;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bytes;
	uint32_t page_count;

	if (!server_registered() || !conn_valid(conn_id))
		return -1;
	if (g_conns[conn_id].is_server)
		return -1;
	if (!window_out || !surface_out)
		return -1;
	if (w <= 0 || h <= 0)
		return -1;
	width = (uint32_t)w;
	height = (uint32_t)h;
	if (width > DRWIN_MAX_WIDTH || height > DRWIN_MAX_HEIGHT)
		return -1;

	for (uint32_t i = 0; i < WMDEV_MAX_WINDOWS; i++) {
		if (!g_windows[i].in_use) {
			win = &g_windows[i];
			break;
		}
	}
	if (!win)
		return -1;

	pitch = width * 4u;
	bytes = pitch * height;
	page_count = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;

	k_memset(win, 0, sizeof(*win));
	win->in_use = 1;
	win->owner_pid = g_conns[conn_id].pid;
	win->owner_conn = conn_id;
	win->id = g_next_window_id++;
	win->rect.x = x;
	win->rect.y = y;
	win->rect.w = w;
	win->rect.h = h;
	win->width = width;
	win->height = height;
	win->pitch = pitch;
	win->page_count = page_count;
	k_strncpy(win->title, title ? title : "", DRWIN_MAX_TITLE - 1u);
	win->title[DRWIN_MAX_TITLE - 1u] = '\0';

	for (uint32_t i = 0; i < page_count; i++) {
		void *page;
		win->pages[i] = pmm_alloc_page();
		if (!win->pages[i]) {
			free_window_pages(win);
			k_memset(win, 0, sizeof(*win));
			return -1;
		}
		page = arch_page_temp_map(win->pages[i]);
		if (!page) {
			free_window_pages(win);
			k_memset(win, 0, sizeof(*win));
			return -1;
		}
		k_memset(page, 0, PAGE_SIZE);
		arch_page_temp_unmap(page);
	}

	fill_surface(&surface, win->id, width, height, pitch);
	k_memset(&msg, 0, sizeof(msg));
	msg.size = sizeof(drwin_server_msg_t);
	msg.type = DRWIN_MSG_CREATE_WINDOW;
	msg.window = win->id;
	msg.rect = win->rect;
	msg.surface = surface;
	k_strncpy(msg.title, win->title, DRWIN_MAX_TITLE - 1u);
	msg.title[DRWIN_MAX_TITLE - 1u] = '\0';
	if (enqueue_server_msg(&msg) != 0) {
		free_window_pages(win);
		k_memset(win, 0, sizeof(*win));
		return -1;
	}

	*window_out = win->id;
	*surface_out = surface;
	return 0;
}

int wmdev_present_window(uint32_t conn_id, uint32_t window, drwin_rect_t dirty)
{
	wmdev_window_t *win;
	drwin_server_msg_t msg;

	if (!conn_valid(conn_id))
		return -1;
	win = find_window(window);
	if (!win || win->owner_conn != conn_id)
		return -1;

	k_memset(&msg, 0, sizeof(msg));
	msg.size = sizeof(drwin_server_msg_t);
	msg.type = DRWIN_MSG_PRESENT_WINDOW;
	msg.window = window;
	msg.rect = dirty;
	fill_surface(&msg.surface, win->id, win->width, win->height, win->pitch);
	return enqueue_server_msg(&msg);
}

int wmdev_destroy_window(uint32_t conn_id, uint32_t window)
{
	wmdev_window_t *win;
	drwin_server_msg_t msg;

	if (!conn_valid(conn_id))
		return -1;
	win = find_window(window);
	if (!win || win->owner_conn != conn_id)
		return -1;

	remove_server_msgs_for_window(window);
	make_room_for_authoritative_server_msg();
	k_memset(&msg, 0, sizeof(msg));
	msg.size = sizeof(drwin_server_msg_t);
	msg.type = DRWIN_MSG_DESTROY_WINDOW;
	msg.window = window;
	msg.rect = win->rect;
	if (enqueue_server_msg(&msg) != 0)
		return -1;

	free_window_pages(win);
	k_memset(win, 0, sizeof(*win));
	return 0;
}

int wmdev_queue_event(uint32_t window, const drwin_event_t *event)
{
	wmdev_window_t *win = find_window(window);

	if (!win || !event)
		return -1;
	return enqueue_event_to_conn(win->owner_conn, event);
}

int wmdev_read_event(uint32_t conn_id, drwin_event_t *event_out)
{
	wmdev_conn_t *conn;

	if (!conn_valid(conn_id) || !event_out)
		return -1;
	conn = &g_conns[conn_id];
	if (conn->event_count == 0)
		return -1;

	*event_out = conn->events[conn->event_tail];
	conn->event_tail = (conn->event_tail + 1u) % WMDEV_EVENT_QUEUE_CAP;
	conn->event_count--;
	return 0;
}

int wmdev_read_server_msg(uint32_t conn_id, drwin_server_msg_t *msg_out)
{
	if (!msg_out || !server_registered() || conn_id != g_server_conn)
		return -1;
	if (g_server_count == 0)
		return -1;

	*msg_out = g_server_msgs[g_server_tail];
	g_server_tail = (g_server_tail + 1u) % WMDEV_EVENT_QUEUE_CAP;
	g_server_count--;
	return 0;
}

int wmdev_mmap_page(uint32_t conn_id,
                    uint32_t map_offset,
                    uint32_t page_index,
                    uint32_t *phys_out)
{
	wmdev_window_t *win;

	if (!conn_valid(conn_id) || !phys_out)
		return -1;
	if ((map_offset & (PAGE_SIZE - 1u)) != 0)
		return -1;
	win = find_window_by_map_offset(map_offset);
	if (!win)
		return -1;
	if (win->owner_conn != conn_id &&
	    (!server_registered() || conn_id != g_server_conn))
		return -1;
	if (page_index >= win->page_count || win->pages[page_index] == 0)
		return -1;

	*phys_out = win->pages[page_index];
	return 0;
}

int wmdev_read_user_record(uint32_t conn_id, uint8_t *buf, uint32_t count)
{
	wmdev_conn_t *conn;

	if (!conn_valid(conn_id) || !buf)
		return -1;
	if (server_registered() && conn_id == g_server_conn) {
		drwin_server_msg_t msg;

		if (count < sizeof(msg))
			return -1;
		if (wmdev_read_server_msg(conn_id, &msg) != 0)
			return -1;
		k_memcpy(buf, &msg, sizeof(msg));
		return (int)sizeof(msg);
	}

	conn = &g_conns[conn_id];
	if (conn->record_count != 0) {
		uint32_t size = conn->record_sizes[conn->record_tail];

		if (count < size)
			return -1;
		k_memcpy(buf, conn->records[conn->record_tail], size);
		k_memset(conn->records[conn->record_tail],
		         0,
		         sizeof(conn->records[conn->record_tail]));
		conn->record_sizes[conn->record_tail] = 0;
		conn->record_tail =
		    (conn->record_tail + 1u) % WMDEV_EVENT_QUEUE_CAP;
		conn->record_count--;
		return (int)size;
	}

	if (conn->event_count != 0) {
		drwin_event_t event;

		if (count < sizeof(event))
			return -1;
		if (wmdev_read_event(conn_id, &event) != 0)
			return -1;
		k_memcpy(buf, &event, sizeof(event));
		return (int)sizeof(event);
	}

	return -1;
}

static int read_record_header(const uint8_t *buf,
                              uint32_t count,
                              uint32_t *size_out,
                              uint32_t *type_out)
{
	uint32_t size;
	uint32_t type;

	if (!buf || !size_out || !type_out || count < sizeof(uint32_t) * 2u)
		return -1;
	k_memcpy(&size, buf, sizeof(size));
	k_memcpy(&type, buf + sizeof(size), sizeof(type));
	if (size != count)
		return -1;
	*size_out = size;
	*type_out = type;
	return 0;
}

int wmdev_write_user_record(uint32_t conn_id, const uint8_t *buf, uint32_t count)
{
	uint32_t size;
	uint32_t type;

	if (!conn_valid(conn_id) ||
	    read_record_header(buf, count, &size, &type) != 0)
		return -1;

	if (type == DRWIN_REQ_REGISTER_SERVER) {
		drwin_register_server_request_t req;

		if (size != sizeof(req))
			return -1;
		k_memcpy(&req, buf, sizeof(req));
		if (wmdev_register_server(conn_id, req.magic) != 0)
			return -1;
		return (int)size;
	}

	if (g_conns[conn_id].is_server) {
		drwin_send_event_request_t req;

		if (type != DRWIN_REQ_SEND_EVENT || size != sizeof(req))
			return -1;
		k_memcpy(&req, buf, sizeof(req));
		if (wmdev_queue_event((uint32_t)req.event.window, &req.event) != 0)
			return -1;
		return (int)size;
	}

	switch (type) {
	case DRWIN_REQ_CREATE_WINDOW: {
		drwin_create_window_request_t req;
		drwin_create_window_response_t resp;

		if (size != sizeof(req))
			return -1;
		if (g_conns[conn_id].record_count >= WMDEV_EVENT_QUEUE_CAP)
			return -1;
		k_memcpy(&req, buf, sizeof(req));
		k_memset(&resp, 0, sizeof(resp));
		resp.size = sizeof(resp);
		resp.type = DRWIN_CLIENT_MSG_CREATE_WINDOW_RESPONSE;
		if (wmdev_create_window(conn_id,
		                        req.title,
		                        req.x,
		                        req.y,
		                        req.w,
		                        req.h,
		                        &resp.window,
		                        &resp.surface) != 0)
			return -1;
		resp.status = 0;
		if (enqueue_client_record(conn_id, &resp, sizeof(resp)) != 0)
			return -1;
		return (int)size;
	}
	case DRWIN_REQ_DESTROY_WINDOW: {
		drwin_window_rect_request_t req;

		if (size != sizeof(req))
			return -1;
		k_memcpy(&req, buf, sizeof(req));
		return wmdev_destroy_window(conn_id, req.window) == 0 ? (int)size
		                                                     : -1;
	}
	case DRWIN_REQ_PRESENT_WINDOW: {
		drwin_window_rect_request_t req;

		if (size != sizeof(req))
			return -1;
		k_memcpy(&req, buf, sizeof(req));
		return wmdev_present_window(conn_id, req.window, req.rect) == 0
		           ? (int)size
		           : -1;
	}
	case DRWIN_REQ_SET_TITLE: {
		drwin_set_title_request_t req;
		drwin_rect_t zero = {0, 0, 0, 0};

		if (size != sizeof(req))
			return -1;
		k_memcpy(&req, buf, sizeof(req));
		return enqueue_surface_update(conn_id,
		                              req.window,
		                              DRWIN_MSG_SET_TITLE,
		                              zero,
		                              req.title,
		                              0) == 0
		           ? (int)size
		           : -1;
	}
	case DRWIN_REQ_SHOW_WINDOW: {
		drwin_show_window_request_t req;
		drwin_rect_t zero = {0, 0, 0, 0};

		if (size != sizeof(req))
			return -1;
		k_memcpy(&req, buf, sizeof(req));
		return enqueue_surface_update(conn_id,
		                              req.window,
		                              DRWIN_MSG_SHOW_WINDOW,
		                              zero,
		                              0,
		                              req.visible) == 0
		           ? (int)size
		           : -1;
	}
	default:
		return -1;
	}
}

int wmdev_event_available(uint32_t conn_id)
{
	if (!conn_valid(conn_id))
		return 0;
	return g_conns[conn_id].event_count != 0 ||
	       g_conns[conn_id].record_count != 0;
}

int wmdev_server_msg_available(uint32_t conn_id)
{
	if (!server_registered() || conn_id != g_server_conn)
		return 0;
	return g_server_count != 0;
}

int wmdev_window_owner_for_test(uint32_t window)
{
	wmdev_window_t *win = find_window(window);
	if (!win)
		return -1;
	return (int)win->owner_pid;
}

uint32_t wmdev_window_page_count_for_test(uint32_t window)
{
	wmdev_window_t *win = find_window(window);
	if (!win)
		return 0;
	return win->page_count;
}

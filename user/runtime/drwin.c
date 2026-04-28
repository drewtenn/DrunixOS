/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "drwin.h"
#include "mman.h"
#include "string.h"
#include "syscall.h"

#define DRWIN_SURFACE_TABLE_SIZE 32

typedef struct {
	int in_use;
	drwin_window_t window;
	drwin_surface_info_t info;
	void *pixels;
	unsigned int mapped_length;
} drwin_surface_slot_t;

static int g_drwin_fd = -1;
static drwin_surface_slot_t g_surfaces[DRWIN_SURFACE_TABLE_SIZE];

static void copy_title(char dst[DRWIN_MAX_TITLE], const char *title)
{
	unsigned int i;

	if (!title)
		title = "";
	for (i = 0; i + 1u < DRWIN_MAX_TITLE && title[i]; i++)
		dst[i] = title[i];
	dst[i] = '\0';
	while (++i < DRWIN_MAX_TITLE)
		dst[i] = '\0';
}

static int write_all(const void *buf, unsigned int count)
{
	const char *p = (const char *)buf;
	unsigned int done = 0;

	while (done < count) {
		int n = sys_fwrite(g_drwin_fd, p + done, (int)(count - done));
		if (n <= 0)
			return -1;
		done += (unsigned int)n;
	}
	return 0;
}

static int read_all(void *buf, unsigned int count)
{
	char *p = (char *)buf;
	unsigned int done = 0;

	while (done < count) {
		int n = sys_read(g_drwin_fd, p + done, (int)(count - done));
		if (n <= 0)
			return -1;
		done += (unsigned int)n;
	}
	return 0;
}

static drwin_surface_slot_t *find_surface(drwin_window_t window)
{
	for (int i = 0; i < DRWIN_SURFACE_TABLE_SIZE; i++) {
		if (g_surfaces[i].in_use && g_surfaces[i].window == window)
			return &g_surfaces[i];
	}
	return 0;
}

static drwin_surface_slot_t *alloc_surface(drwin_window_t window)
{
	drwin_surface_slot_t *slot = find_surface(window);

	if (slot)
		return slot;
	for (int i = 0; i < DRWIN_SURFACE_TABLE_SIZE; i++) {
		if (!g_surfaces[i].in_use) {
			memset(&g_surfaces[i], 0, sizeof(g_surfaces[i]));
			g_surfaces[i].in_use = 1;
			g_surfaces[i].window = window;
			return &g_surfaces[i];
		}
	}
	return 0;
}

static int has_surface_capacity(void)
{
	for (int i = 0; i < DRWIN_SURFACE_TABLE_SIZE; i++) {
		if (!g_surfaces[i].in_use)
			return 1;
	}
	return 0;
}

static int request_destroy_window(drwin_window_t window)
{
	drwin_window_rect_request_t req;

	if (window <= 0 || g_drwin_fd < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_DESTROY_WINDOW;
	req.window = (uint32_t)window;
	return write_all(&req, sizeof(req));
}

static unsigned int surface_length(const drwin_surface_info_t *info)
{
	if (!info || info->pitch == 0 || info->height == 0)
		return 0;
	if (info->pitch > ((unsigned int)-1) / info->height)
		return 0;
	return info->pitch * info->height;
}

static void unmap_surface(drwin_surface_slot_t *slot)
{
	if (!slot || !slot->pixels || slot->mapped_length == 0)
		return;
	(void)sys_munmap(slot->pixels, slot->mapped_length);
	slot->pixels = 0;
	slot->mapped_length = 0;
}

static void forget_surface(drwin_window_t window)
{
	drwin_surface_slot_t *slot = find_surface(window);

	if (slot) {
		unmap_surface(slot);
		memset(slot, 0, sizeof(*slot));
	}
}

int drwin_connect(void)
{
	if (g_drwin_fd >= 0)
		return 0;
	g_drwin_fd = sys_open_flags("/dev/wm", SYS_O_RDWR, 0);
	return g_drwin_fd >= 0 ? 0 : -1;
}

int drwin_create_window(const char *title,
                        int x,
                        int y,
                        int w,
                        int h,
                        drwin_window_t *out)
{
	drwin_create_window_request_t req;
	drwin_create_window_response_t resp;
	drwin_surface_slot_t *slot;

	if (!out || w <= 0 || h <= 0 || w > DRWIN_MAX_WIDTH ||
	    h > DRWIN_MAX_HEIGHT)
		return -1;
	if (drwin_connect() != 0)
		return -1;
	if (!has_surface_capacity())
		return -1;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_CREATE_WINDOW;
	req.x = x;
	req.y = y;
	req.w = w;
	req.h = h;
	copy_title(req.title, title);
	if (write_all(&req, sizeof(req)) != 0)
		return -1;

	memset(&resp, 0, sizeof(resp));
	if (read_all(&resp, sizeof(resp)) != 0)
		return -1;
	if (resp.size != sizeof(resp) ||
	    resp.type != DRWIN_CLIENT_MSG_CREATE_WINDOW_RESPONSE ||
	    resp.status != 0 || resp.window == 0)
		return -1;

	slot = alloc_surface((drwin_window_t)resp.window);
	if (!slot) {
		(void)request_destroy_window((drwin_window_t)resp.window);
		return -1;
	}
	slot->info = resp.surface;
	slot->pixels = 0;
	*out = (drwin_window_t)resp.window;
	return 0;
}

int drwin_map_surface(drwin_window_t window, drwin_surface_t *out)
{
	drwin_surface_slot_t *slot;
	unsigned int length;

	if (!out || window <= 0)
		return -1;
	if (drwin_connect() != 0)
		return -1;
	slot = find_surface(window);
	if (!slot || slot->info.width == 0 || slot->info.height == 0 ||
	    slot->info.pitch == 0 || slot->info.bpp != DRWIN_BPP)
		return -1;
	length = surface_length(&slot->info);
	if (length == 0)
		return -1;
	if (!slot->pixels) {
		slot->pixels = mmap(0,
		                    length,
		                    PROT_READ | PROT_WRITE,
		                    MAP_SHARED,
		                    g_drwin_fd,
		                    slot->info.map_offset);
		if (slot->pixels == MAP_FAILED) {
			slot->pixels = 0;
			return -1;
		}
		slot->mapped_length = length;
	}

	out->width = (int)slot->info.width;
	out->height = (int)slot->info.height;
	out->pitch = (int)slot->info.pitch;
	out->bpp = (int)slot->info.bpp;
	out->pixels = slot->pixels;
	return 0;
}

int drwin_present(drwin_window_t window, drwin_rect_t dirty)
{
	drwin_window_rect_request_t req;

	if (window <= 0 || dirty.w < 0 || dirty.h < 0)
		return -1;
	if (drwin_connect() != 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_PRESENT_WINDOW;
	req.window = (uint32_t)window;
	req.rect = dirty;
	return write_all(&req, sizeof(req));
}

int drwin_poll_event(drwin_event_t *event, int timeout_ms)
{
	sys_pollfd_t pfd;

	if (!event)
		return -1;
	if (drwin_connect() != 0)
		return -1;
	pfd.fd = g_drwin_fd;
	pfd.events = SYS_POLLIN;
	pfd.revents = 0;
	int ready = sys_poll(&pfd, 1, timeout_ms);
	if (ready < 0)
		return -1;
	if (ready == 0)
		return 0;
	if (!(pfd.revents & SYS_POLLIN))
		return -1;
	if (read_all(event, sizeof(*event)) != 0)
		return -1;
	return 1;
}

int drwin_set_title(drwin_window_t window, const char *title)
{
	drwin_set_title_request_t req;

	if (window <= 0)
		return -1;
	if (drwin_connect() != 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_SET_TITLE;
	req.window = (uint32_t)window;
	copy_title(req.title, title);
	return write_all(&req, sizeof(req));
}

int drwin_show_window(drwin_window_t window, int visible)
{
	drwin_show_window_request_t req;

	if (window <= 0)
		return -1;
	if (drwin_connect() != 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_SHOW_WINDOW;
	req.window = (uint32_t)window;
	req.visible = visible ? 1u : 0u;
	return write_all(&req, sizeof(req));
}

int drwin_destroy_window(drwin_window_t window)
{
	if (window <= 0)
		return -1;
	if (drwin_connect() != 0)
		return -1;
	if (request_destroy_window(window) != 0)
		return -1;
	forget_surface(window);
	return 0;
}

int drwin_fd_for_poll(void)
{
	if (drwin_connect() != 0)
		return -1;
	return g_drwin_fd;
}

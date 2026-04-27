/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRUNIX_WM_API_H
#define DRUNIX_WM_API_H

#include <stdint.h>

#define DRWIN_MAX_TITLE 64
#define DRWIN_MAX_WIDTH 1024
#define DRWIN_MAX_HEIGHT 768
#define DRWIN_BPP 32
#define DRWIN_SERVER_MAGIC 0x44574d53u

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

#endif /* DRUNIX_WM_API_H */

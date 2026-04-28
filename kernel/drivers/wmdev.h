/* SPDX-License-Identifier: GPL-3.0-or-later */

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
int wmdev_retain(uint32_t conn_id);
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
int wmdev_mmap_page(uint32_t conn_id,
                    uint32_t map_offset,
                    uint32_t page_index,
                    uint32_t *phys_out);
int wmdev_read_user_record(uint32_t conn_id, uint8_t *buf, uint32_t count);
int wmdev_write_user_record(uint32_t conn_id,
                            const uint8_t *buf,
                            uint32_t count);
int wmdev_event_available(uint32_t conn_id);
int wmdev_server_msg_available(uint32_t conn_id);
int wmdev_window_owner_for_test(uint32_t window);
uint32_t wmdev_window_page_count_for_test(uint32_t window);

#endif

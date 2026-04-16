#ifndef GUI_DESKTOP_APPS_H
#define GUI_DESKTOP_APPS_H

#include "desktop_app_types.h"
#include "framebuffer.h"
#include "pixel.h"
#include <stdint.h>

typedef enum {
    DESKTOP_APP_KEY_IGNORED = 0,
    DESKTOP_APP_KEY_HANDLED = 1,
    DESKTOP_APP_KEY_CLOSE = 2,
} desktop_app_key_result_t;

void desktop_apps_init(desktop_app_state_t *state);
void desktop_app_refresh(desktop_app_state_t *state);
void desktop_app_render(const desktop_app_state_t *state,
                        desktop_app_kind_t app,
                        const gui_pixel_surface_t *surface,
                        const gui_pixel_theme_t *theme,
                        const gui_pixel_rect_t *rect);
desktop_app_key_result_t desktop_app_handle_key(desktop_app_state_t *state,
                                                desktop_app_kind_t app,
                                                uint32_t key);

#ifdef KTEST_ENABLED
const char *desktop_app_line_for_test(const desktop_app_view_t *view,
                                      int line);
void desktop_app_refresh_files_for_test(desktop_app_view_t *view,
                                        const char *dents,
                                        int n);
void desktop_app_refresh_processes_for_test(desktop_app_view_t *view);
void desktop_app_refresh_help_for_test(desktop_app_view_t *view);
#endif

#endif /* GUI_DESKTOP_APPS_H */

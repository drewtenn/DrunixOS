#ifndef GUI_DESKTOP_APPS_H
#define GUI_DESKTOP_APPS_H

#include "desktop_app_types.h"
#include "desktop.h"
#include "pixel.h"
#include <stdint.h>

void desktop_apps_init(desktop_app_state_t *state);
void desktop_app_refresh(desktop_app_state_t *state);
void desktop_app_render(const desktop_app_state_t *state,
                        desktop_app_kind_t app,
                        gui_display_t *display,
                        const gui_rect_t *rect);
int desktop_app_handle_key(desktop_app_state_t *state,
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

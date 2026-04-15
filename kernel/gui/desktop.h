#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include "display.h"
#include <stdint.h>

typedef enum {
    DESKTOP_FOCUS_SHELL = 0,
    DESKTOP_FOCUS_TASKBAR = 1,
    DESKTOP_FOCUS_LAUNCHER = 2,
} desktop_focus_t;

typedef struct {
    int x;
    int y;
    int dx;
    int dy;
    int left_down;
} desktop_pointer_event_t;

typedef struct {
    int active;
    int launcher_open;
    int shell_window_open;
    int desktop_enabled;
    desktop_focus_t focus;
    gui_rect_t taskbar;
    gui_rect_t launcher_rect;
    gui_rect_t shell_rect;
    gui_rect_t shell_content;
    gui_display_t *display;
    uintptr_t video_address;
    uint32_t shell_pid;
} desktop_state_t;

typedef enum {
    DESKTOP_KEY_FORWARD = 0,
    DESKTOP_KEY_CONSUMED = 1,
} desktop_key_result_t;

void desktop_init(desktop_state_t *desktop, gui_display_t *display);
void desktop_render(desktop_state_t *desktop);
void desktop_open_shell_window(desktop_state_t *desktop);
void desktop_handle_pointer(desktop_state_t *desktop,
                            const desktop_pointer_event_t *ev);
desktop_key_result_t desktop_handle_key(desktop_state_t *desktop, char c);
int desktop_is_active(void);
desktop_state_t *desktop_global(void);

#endif /* GUI_DESKTOP_H */

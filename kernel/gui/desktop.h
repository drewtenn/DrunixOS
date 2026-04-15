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
    gui_cell_t *shell_cells;
    int shell_cells_w;
    int shell_cells_h;
    int shell_cursor_x;
    int shell_cursor_y;
} desktop_state_t;

typedef enum {
    DESKTOP_KEY_FORWARD = 0,
    DESKTOP_KEY_CONSUMED = 1,
} desktop_key_result_t;

void desktop_init(desktop_state_t *desktop, gui_display_t *display);
void desktop_render(desktop_state_t *desktop);
void desktop_open_shell_window(desktop_state_t *desktop);
void desktop_attach_shell_pid(desktop_state_t *desktop, uint32_t pid);
int desktop_write_process_output(desktop_state_t *desktop,
                                 uint32_t pid,
                                 const char *buf,
                                 uint32_t len);
int desktop_console_mirror_enabled(void);
desktop_key_result_t desktop_handle_key(desktop_state_t *desktop, char c);
int desktop_is_active(void);
desktop_state_t *desktop_global(void);

#endif /* GUI_DESKTOP_H */

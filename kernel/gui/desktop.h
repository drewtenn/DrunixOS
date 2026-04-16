#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include "display.h"
#include "framebuffer.h"
#include "terminal.h"
#include <stdint.h>

typedef enum {
    DESKTOP_FOCUS_SHELL = 0,
    DESKTOP_FOCUS_TASKBAR = 1,
    DESKTOP_FOCUS_LAUNCHER = 2,
    DESKTOP_FOCUS_WINDOW = 3,
} desktop_focus_t;

#define DESKTOP_MAX_WINDOWS 4
#define DESKTOP_WINDOW_TITLE_MAX 16

typedef enum {
    DESKTOP_APP_NONE = 0,
    DESKTOP_APP_SHELL = 1,
    DESKTOP_APP_FILES = 2,
    DESKTOP_APP_PROCESSES = 3,
    DESKTOP_APP_HELP = 4,
} desktop_app_kind_t;

typedef struct {
    int id;
    int open;
    int focused;
    int z;
    desktop_app_kind_t app;
    char title[DESKTOP_WINDOW_TITLE_MAX];
    gui_pixel_rect_t rect;
    gui_pixel_rect_t content_rect;
} desktop_window_t;

typedef struct {
    int x;
    int y;
    int pixel_x;
    int pixel_y;
    int dx;
    int dy;
    int left_down;
} desktop_pointer_event_t;

typedef struct {
    int active;
    int launcher_open;
    desktop_app_kind_t launcher_selection;
    int shell_window_open;
    int desktop_enabled;
    desktop_focus_t focus;
    gui_rect_t taskbar;
    gui_rect_t launcher_rect;
    gui_rect_t shell_rect;
    gui_rect_t shell_content;
    gui_pixel_rect_t taskbar_pixel_rect;
    gui_pixel_rect_t launcher_pixel_rect;
    gui_pixel_rect_t window_pixel_rect;
    gui_pixel_rect_t shell_pixel_rect;
    gui_display_t *display;
    uintptr_t video_address;
    const framebuffer_info_t *framebuffer;
    int framebuffer_enabled;
    uint32_t shell_pid;
    uint32_t shell_pgid;
    gui_terminal_t shell_terminal;
    desktop_window_t windows[DESKTOP_MAX_WINDOWS];
    int next_window_id;
    int focused_window_id;
    int next_z;
    int dragging_window_id;
    int drag_offset_x;
    int drag_offset_y;
    gui_cell_t *shell_cells;
    int shell_cells_w;
    int shell_cells_h;
    int shell_cursor_x;
    int shell_cursor_y;
    int shell_wrap_pending;
    int shell_ansi_state;
    int shell_ansi_val;
    uint8_t shell_attr;
    int pointer_x;
    int pointer_y;
    int pointer_pixel_x;
    int pointer_pixel_y;
    int pointer_visible;
} desktop_state_t;

typedef enum {
    DESKTOP_KEY_FORWARD = 0,
    DESKTOP_KEY_CONSUMED = 1,
} desktop_key_result_t;

void desktop_init(desktop_state_t *desktop, gui_display_t *display);
void desktop_render(desktop_state_t *desktop);
void desktop_set_presentation_target(desktop_state_t *desktop,
                                      uintptr_t video_address);
void desktop_set_framebuffer_target(desktop_state_t *desktop,
                                    const framebuffer_info_t *framebuffer);
void desktop_open_shell_window(desktop_state_t *desktop);
int desktop_open_app_window(desktop_state_t *desktop, desktop_app_kind_t app);
int desktop_close_window(desktop_state_t *desktop, int window_id);
void desktop_focus_window(desktop_state_t *desktop, int window_id);
void desktop_attach_shell_pid(desktop_state_t *desktop, uint32_t pid);
void desktop_attach_shell_process(desktop_state_t *desktop, uint32_t pid,
                                  uint32_t pgid);
uint32_t desktop_shell_pid(const desktop_state_t *desktop);
int desktop_process_owns_shell(desktop_state_t *desktop,
                               uint32_t pid,
                               uint32_t pgid);
int desktop_write_console_output(desktop_state_t *desktop,
                                 const char *buf,
                                 uint32_t len);
int desktop_clear_console(desktop_state_t *desktop);
int desktop_scroll_console(desktop_state_t *desktop, int rows);
int desktop_write_process_output(desktop_state_t *desktop,
                                 uint32_t pid,
                                 uint32_t pgid,
                                 const char *buf,
                                 uint32_t len);
int desktop_console_mirror_enabled(void);
void desktop_handle_pointer(desktop_state_t *desktop,
                            const desktop_pointer_event_t *ev);
desktop_key_result_t desktop_handle_key(desktop_state_t *desktop, char c);
int desktop_is_active(void);
desktop_state_t *desktop_global(void);

#ifdef KTEST_ENABLED
typedef void (*desktop_scroll_interleave_hook_t)(desktop_state_t *desktop);
void desktop_set_scroll_interleave_hook_for_test(
    desktop_scroll_interleave_hook_t hook);
int desktop_window_count_for_test(const desktop_state_t *desktop);
desktop_app_kind_t desktop_focused_app_for_test(const desktop_state_t *desktop);
int desktop_window_z_for_test(const desktop_state_t *desktop, int window_id);
void desktop_focus_window_for_test(desktop_state_t *desktop, int window_id);
#endif

#endif /* GUI_DESKTOP_H */

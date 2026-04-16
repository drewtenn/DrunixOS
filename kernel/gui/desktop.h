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
} desktop_focus_t;

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
#endif

#endif /* GUI_DESKTOP_H */

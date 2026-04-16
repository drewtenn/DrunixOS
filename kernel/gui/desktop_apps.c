#include "desktop_apps.h"
#include "kprintf.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"
#include <limits.h>

static int desktop_app_rect_intersect(gui_pixel_rect_t a,
                                      gui_pixel_rect_t b,
                                      gui_pixel_rect_t *out)
{
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    int64_t a_right;
    int64_t a_bottom;
    int64_t b_right;
    int64_t b_bottom;

    if (!out)
        return 0;

    a_right = (int64_t)a.x + (int64_t)a.w;
    a_bottom = (int64_t)a.y + (int64_t)a.h;
    b_right = (int64_t)b.x + (int64_t)b.w;
    b_bottom = (int64_t)b.y + (int64_t)b.h;
    left = a.x > b.x ? a.x : b.x;
    top = a.y > b.y ? a.y : b.y;
    right = a_right < b_right ? a_right : b_right;
    bottom = a_bottom < b_bottom ? a_bottom : b_bottom;
    if (right <= left || bottom <= top)
        return 0;
    if (left < INT_MIN || top < INT_MIN || right > INT_MAX ||
        bottom > INT_MAX)
        return 0;
    out->x = (int)left;
    out->y = (int)top;
    out->w = (int)(right - left);
    out->h = (int)(bottom - top);
    return 1;
}

static int desktop_app_safe_int64_to_int(int64_t value, int *out)
{
    if (!out || value < INT_MIN || value > INT_MAX)
        return 0;
    *out = (int)value;
    return 1;
}

static void desktop_app_clear_view(desktop_app_view_t *view)
{
    if (!view)
        return;
    k_memset(view, 0, sizeof(*view));
}

static void desktop_app_set_line(desktop_app_view_t *view, int line,
                                 const char *text)
{
    uint32_t len = 0;

    if (!view || line < 0 || line >= DESKTOP_APP_MAX_LINES)
        return;

    k_memset(view->lines[line], 0, DESKTOP_APP_LINE_MAX);
    if (text)
        len = k_strnlen(text, DESKTOP_APP_LINE_MAX - 1u);
    if (len > 0)
        k_memcpy(view->lines[line], text, len);
    view->lines[line][len] = '\0';
    if (view->line_count <= line)
        view->line_count = line + 1;
}

static void desktop_app_append_line(desktop_app_view_t *view, const char *text)
{
    if (!view || view->line_count >= DESKTOP_APP_MAX_LINES)
        return;
    desktop_app_set_line(view, view->line_count, text);
}

static void desktop_app_mark_truncated(desktop_app_view_t *view)
{
    if (!view)
        return;

    if (view->line_count < DESKTOP_APP_MAX_LINES)
        desktop_app_append_line(view, "... more entries");
    else
        desktop_app_set_line(view, DESKTOP_APP_MAX_LINES - 1,
                             "... more entries");
}

static void desktop_app_refresh_files_from_dents(desktop_app_view_t *view,
                                                 const char *dents,
                                                 int n)
{
    int i;
    int truncated = 0;

    desktop_app_clear_view(view);
    desktop_app_set_line(view, 0, "Files: /");

    if (!dents || n < 0) {
        desktop_app_append_line(view, "error: cannot list /");
        return;
    }

    for (i = 0; i < n; ) {
        const char *entry = dents + i;
        uint32_t remaining = (uint32_t)(n - i);
        uint32_t len = k_strnlen(entry, remaining);

        if (len == remaining) {
            truncated = 1;
            break;
        }

        if (view->line_count >= DESKTOP_APP_MAX_LINES - 1 &&
            i + (int)len + 1 < n) {
            truncated = 1;
            break;
        }

        desktop_app_append_line(view, entry);
        i += (int)len + 1;
    }

    if (truncated || i < n)
        desktop_app_mark_truncated(view);
}

static const char *desktop_app_process_state_label(const process_t *proc)
{
    if (!proc)
        return "UNKNOWN";

    switch ((proc_state_t)proc->state) {
    case PROC_READY:
        return "READY";
    case PROC_RUNNING:
        return "RUNNING";
    case PROC_ZOMBIE:
        return "ZOMBIE";
    case PROC_BLOCKED:
        return "BLOCKED";
    case PROC_STOPPED:
        return "STOPPED";
    case PROC_UNUSED:
    default:
        return "UNKNOWN";
    }
}

static void desktop_app_refresh_files(desktop_app_view_t *view)
{
    char dents[512];
    int n;

    n = vfs_getdents(0, dents, sizeof(dents));
    desktop_app_refresh_files_from_dents(view, dents, n);
}

static void desktop_app_refresh_processes(desktop_app_view_t *view)
{
    uint32_t pids[DESKTOP_APP_MAX_LINES];
    int n;

    desktop_app_clear_view(view);
    desktop_app_set_line(view, 0, "PID  STATE  NAME");

    n = sched_snapshot_pids(pids, DESKTOP_APP_MAX_LINES, 1);
    for (int i = 0; i < n && view->line_count < DESKTOP_APP_MAX_LINES; i++) {
        const process_t *proc = sched_find_process(pids[i], 1);
        char line[DESKTOP_APP_LINE_MAX];

        if (!proc)
            continue;

        k_snprintf(line, sizeof(line), "%u  %s  %s",
                   proc->pid,
                   desktop_app_process_state_label(proc),
                   proc->name[0] ? proc->name : "?");
        line[DESKTOP_APP_LINE_MAX - 1] = '\0';
        desktop_app_append_line(view, line);
    }
}

static void desktop_app_refresh_help(desktop_app_view_t *view)
{
    desktop_app_clear_view(view);
    desktop_app_set_line(view, 0, "Drunix Help");
    desktop_app_set_line(view, 1, "Keyboard");
    desktop_app_set_line(view, 2, "Esc launcher");
    desktop_app_set_line(view, 3, "1-4 selection");
    desktop_app_set_line(view, 4, "Drag title bars");
    desktop_app_set_line(view, 5, "x close");
    desktop_app_set_line(view, 6, "q close");
    desktop_app_set_line(view, 7, "Shell keeps running");
}

void desktop_apps_init(desktop_app_state_t *state)
{
    if (!state)
        return;

    k_memset(state, 0, sizeof(*state));
    state->files_path[0] = '/';
    state->files_path[1] = '\0';
}

void desktop_app_refresh(desktop_app_state_t *state)
{
    if (!state)
        return;

    desktop_app_refresh_files(&state->files);
    desktop_app_refresh_processes(&state->processes);
    desktop_app_refresh_help(&state->help);
}

void desktop_app_refresh_for_focus(desktop_app_state_t *state,
                                   desktop_app_kind_t app)
{
    if (!state)
        return;

    if (app == DESKTOP_APP_PROCESSES)
        desktop_app_refresh_processes(&state->processes);
    else if (app == DESKTOP_APP_HELP)
        desktop_app_refresh_help(&state->help);
}

void desktop_app_render(const desktop_app_state_t *state,
                        desktop_app_kind_t app,
                        const gui_pixel_surface_t *surface,
                        const gui_pixel_theme_t *theme,
                        const gui_pixel_rect_t *rect)
{
    const desktop_app_view_t *view = 0;
    gui_pixel_rect_t clip;
    int start_line;
    int line;
    int64_t content_left;
    int64_t content_top;
    int64_t content_bottom;
    int64_t line_y;
    int safe_x;
    int safe_y;

    if (!state || !surface || !surface->fb || !theme || !rect)
        return;

    if (app == DESKTOP_APP_FILES)
        view = &state->files;
    else if (app == DESKTOP_APP_PROCESSES)
        view = &state->processes;
    else if (app == DESKTOP_APP_HELP)
        view = &state->help;
    else
        return;

    if (!desktop_app_rect_intersect(surface->clip, *rect, &clip))
        return;

    framebuffer_fill_rect(surface->fb, clip.x, clip.y, clip.w, clip.h,
                          theme->window_bg);

    start_line = view->scroll < 0 ? 0 : view->scroll;
    content_left = (int64_t)rect->x + 6;
    content_top = (int64_t)rect->y + 6;
    content_bottom = (int64_t)rect->y + (int64_t)rect->h;
    line_y = content_top;
    if (!desktop_app_safe_int64_to_int(content_left, &safe_x) ||
        !desktop_app_safe_int64_to_int(line_y, &safe_y))
        return;
    for (line = start_line; line < view->line_count; line++) {
        if (line_y + (int64_t)GUI_FONT_H > content_bottom)
            break;
        if (!desktop_app_safe_int64_to_int(line_y, &safe_y))
            return;
        framebuffer_draw_text_clipped(surface->fb, &clip,
                                      safe_x,
                                      safe_y,
                                      view->lines[line],
                                      theme->title_fg,
                                      theme->window_bg);
        line_y += (int64_t)GUI_FONT_H;
    }
}

desktop_app_key_result_t desktop_app_handle_key(desktop_app_state_t *state,
                                                desktop_app_kind_t app,
                                                uint32_t key)
{
    desktop_app_view_t *view = 0;

    if (!state)
        return DESKTOP_APP_KEY_IGNORED;

    if (app == DESKTOP_APP_FILES)
        view = &state->files;
    else if (app == DESKTOP_APP_PROCESSES)
        view = &state->processes;
    else if (app == DESKTOP_APP_HELP)
        view = &state->help;
    else
        return DESKTOP_APP_KEY_IGNORED;

    if (key == 'q' || key == 27)
        return DESKTOP_APP_KEY_CLOSE;

    if (key == 'j') {
        if (view->scroll < view->line_count - 1) {
            view->scroll++;
            if (view->scroll < 0)
                view->scroll = 0;
            return DESKTOP_APP_KEY_HANDLED;
        }
        return DESKTOP_APP_KEY_IGNORED;
    }

    if (key == 'k') {
        if (view->scroll > 0) {
            view->scroll--;
            return DESKTOP_APP_KEY_HANDLED;
        }
        return DESKTOP_APP_KEY_IGNORED;
    }

    return DESKTOP_APP_KEY_IGNORED;
}

#ifdef KTEST_ENABLED
const char *desktop_app_line_for_test(const desktop_app_view_t *view, int line)
{
    if (!view || line < 0 || line >= view->line_count)
        return 0;
    return view->lines[line];
}

void desktop_app_refresh_files_for_test(desktop_app_view_t *view,
                                        const char *dents,
                                        int n)
{
    desktop_app_refresh_files_from_dents(view, dents, n);
}

void desktop_app_refresh_processes_for_test(desktop_app_view_t *view)
{
    desktop_app_refresh_processes(view);
}

void desktop_app_refresh_help_for_test(desktop_app_view_t *view)
{
    desktop_app_refresh_help(view);
}
#endif

#include "desktop_apps.h"
#include "kprintf.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"

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
    int i;
    int truncated = 0;

    desktop_app_clear_view(view);
    desktop_app_set_line(view, 0, "Files: /");

    n = vfs_getdents("/", dents, sizeof(dents));
    if (n < 0) {
        desktop_app_append_line(view, "error: cannot list /");
        return;
    }

    for (i = 0; i < n; ) {
        const char *entry = dents + i;
        uint32_t len = k_strlen(entry);

        if (view->line_count >= DESKTOP_APP_MAX_LINES - 1 &&
            i + (int)len + 1 < n) {
            truncated = 1;
            break;
        }

        desktop_app_append_line(view, entry);
        i += (int)len + 1;
    }

    if ((truncated || i < n) && view->line_count < DESKTOP_APP_MAX_LINES)
        desktop_app_append_line(view, "... more entries");
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

void desktop_app_render(const desktop_app_state_t *state,
                        desktop_app_kind_t app,
                        gui_display_t *display,
                        const gui_rect_t *rect)
{
    (void)state;
    (void)app;
    (void)display;
    (void)rect;
}

int desktop_app_handle_key(desktop_app_state_t *state,
                           desktop_app_kind_t app,
                           uint32_t key)
{
    (void)state;
    (void)app;
    (void)key;
    return 0;
}

#ifdef KTEST_ENABLED
const char *desktop_app_line_for_test(const desktop_app_view_t *view, int line)
{
    if (!view || line < 0 || line >= view->line_count)
        return 0;
    return view->lines[line];
}
#endif

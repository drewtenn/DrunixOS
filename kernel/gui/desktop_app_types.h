#ifndef GUI_DESKTOP_APP_TYPES_H
#define GUI_DESKTOP_APP_TYPES_H

#define DESKTOP_APP_MAX_LINES 32
#define DESKTOP_APP_LINE_MAX 64

typedef struct {
    char lines[DESKTOP_APP_MAX_LINES][DESKTOP_APP_LINE_MAX];
    int line_count;
    int scroll;
    int selected;
} desktop_app_view_t;

typedef struct {
    desktop_app_view_t files;
    desktop_app_view_t processes;
    desktop_app_view_t help;
    char files_path[128];
} desktop_app_state_t;

#endif /* GUI_DESKTOP_APP_TYPES_H */

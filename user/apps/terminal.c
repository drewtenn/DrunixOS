/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "drwin.h"
#include "drwin_gfx.h"
#include "string.h"
#include "syscall.h"

#define TERM_COLS 80
#define TERM_ROWS 24
#define TERM_W (TERM_COLS * DRWIN_GLYPH_W + 16)
#define TERM_H (TERM_ROWS * DRWIN_GLYPH_H + 16)
#define TERM_BG 0x00141414u
#define TERM_FG 0x00d0d0d0u

static char g_grid[TERM_ROWS][TERM_COLS];
static int g_cursor_x;
static int g_cursor_y;
static int g_esc_state;
static int g_ptmx = -1;
static int g_shell_pid = -1;

static void term_clear(void)
{
	for (int r = 0; r < TERM_ROWS; r++)
		for (int c = 0; c < TERM_COLS; c++)
			g_grid[r][c] = ' ';
	g_cursor_x = 0;
	g_cursor_y = 0;
	g_esc_state = 0;
}

static void term_scroll(void)
{
	for (int r = 0; r < TERM_ROWS - 1; r++)
		memcpy(g_grid[r], g_grid[r + 1], TERM_COLS);
	for (int c = 0; c < TERM_COLS; c++)
		g_grid[TERM_ROWS - 1][c] = ' ';
}

static void term_putchar(char ch)
{
	if (g_esc_state == 1) {
		g_esc_state = ch == '[' ? 2 : 0;
		return;
	}
	if (g_esc_state == 2) {
		if (ch >= '@' && ch <= '~')
			g_esc_state = 0;
		return;
	}
	if (ch == 0x1b) {
		g_esc_state = 1;
		return;
	}
	if (ch == '\n') {
		g_cursor_x = 0;
		g_cursor_y++;
	} else if (ch == '\r') {
		g_cursor_x = 0;
	} else if (ch == '\b' || ch == 0x7f) {
		if (g_cursor_x > 0) {
			g_cursor_x--;
			g_grid[g_cursor_y][g_cursor_x] = ' ';
		}
	} else if (ch == '\t') {
		int next = (g_cursor_x + 8) & ~7;

		if (next > TERM_COLS)
			next = TERM_COLS;
		while (g_cursor_x < next)
			g_grid[g_cursor_y][g_cursor_x++] = ' ';
	} else if ((unsigned char)ch >= 32 && (unsigned char)ch < 127) {
		if (g_cursor_x >= TERM_COLS) {
			g_cursor_x = 0;
			g_cursor_y++;
		}
		if (g_cursor_y >= TERM_ROWS) {
			term_scroll();
			g_cursor_y = TERM_ROWS - 1;
		}
		g_grid[g_cursor_y][g_cursor_x++] = ch;
	}
	if (g_cursor_y >= TERM_ROWS) {
		term_scroll();
		g_cursor_y = TERM_ROWS - 1;
	}
}

static void render_terminal(drwin_window_t win, drwin_surface_t *surface)
{
	int x0 = 8;
	int y0 = 8;

	drwin_fill_rect(surface, 0, 0, surface->width, surface->height, TERM_BG);
	for (int r = 0; r < TERM_ROWS; r++) {
		for (int c = 0; c < TERM_COLS; c++) {
			char cell[2];

			cell[0] = g_grid[r][c];
			cell[1] = '\0';
			drwin_draw_text(surface,
			                x0 + c * DRWIN_GLYPH_W,
			                y0 + r * DRWIN_GLYPH_H,
			                cell,
			                TERM_FG,
			                TERM_BG);
		}
	}
	drwin_fill_rect(surface,
	                x0 + g_cursor_x * DRWIN_GLYPH_W,
	                y0 + g_cursor_y * DRWIN_GLYPH_H + DRWIN_GLYPH_H - 2,
	                DRWIN_GLYPH_W,
	                2,
	                TERM_FG);
	drwin_present(win, (drwin_rect_t){0, 0, surface->width, surface->height});
}

static int spawn_shell(int slave_fd)
{
	char *shell_argv[] = {"shell", 0};
	char *shell_envp[] = {"PATH=/usr/bin:/bin", 0};
	int pid = sys_fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		sys_dup2(slave_fd, 0);
		sys_dup2(slave_fd, 1);
		sys_dup2(slave_fd, 2);
		if (slave_fd > 2)
			sys_close(slave_fd);
		if (g_ptmx > 2)
			sys_close(g_ptmx);
		{
			int wm_fd = drwin_fd_for_poll();

			if (wm_fd > 2)
				sys_close(wm_fd);
		}
		sys_execve("/bin/shell", shell_argv, shell_envp);
		sys_exit(1);
	}
	return pid;
}

static int open_allocated_slave(void)
{
	unsigned int pty_idx = 0;
	char path[] = "/dev/pts0";

	if (sys_ioctl(g_ptmx, SYS_TIOCGPTN, &pty_idx) != 0)
		return -1;
	if (pty_idx > 9)
		return -1;
	path[8] = (char)('0' + pty_idx);
	return sys_open_flags(path, SYS_O_RDWR, 0);
}

static int start_terminal_session(void)
{
	int slave;

	g_ptmx = sys_open_flags("/dev/ptmx", SYS_O_RDWR, 0);
	if (g_ptmx < 0)
		return -1;
	slave = open_allocated_slave();
	if (slave < 0) {
		sys_close(g_ptmx);
		g_ptmx = -1;
		return -1;
	}
	g_shell_pid = spawn_shell(slave);
	sys_close(slave);
	if (g_shell_pid <= 0) {
		sys_close(g_ptmx);
		g_ptmx = -1;
		return -1;
	}
	return 0;
}

static int fd_has_input(int fd)
{
	sys_pollfd_t pfd;

	pfd.fd = fd;
	pfd.events = SYS_POLLIN;
	pfd.revents = 0;
	return sys_poll(&pfd, 1, 0) > 0 && (pfd.revents & SYS_POLLIN);
}

static int drain_pty_and_present(drwin_window_t win, drwin_surface_t *surface)
{
	char buf[256];
	int rendered = 0;

	for (;;) {
		int n = sys_read(g_ptmx, buf, (int)sizeof(buf));

		if (n == 0)
			return -1;
		if (n < 0)
			return rendered ? 0 : -1;
		for (int i = 0; i < n; i++)
			term_putchar(buf[i]);
		rendered = 1;
		if (!fd_has_input(g_ptmx))
			break;
	}
	if (rendered)
		render_terminal(win, surface);
	return 0;
}

static int event_byte(const drwin_event_t *event)
{
	if (event->value > 0 && event->value <= 0xff)
		return event->value;
	if (event->code > 0 && event->code <= 0xff)
		return event->code;
	return 0;
}

static int drain_window_events(void)
{
	drwin_event_t event;

	while (drwin_poll_event(&event, 0) > 0) {
		if (event.type == DRWIN_EVENT_CLOSE ||
		    event.type == DRWIN_EVENT_DISCONNECT)
			return -1;
		if (event.type == DRWIN_EVENT_KEY && g_ptmx >= 0) {
			char ch = (char)event_byte(&event);

			if (ch)
				sys_fwrite(g_ptmx, &ch, 1);
		}
	}
	return 0;
}

static void stop_terminal_session(void)
{
	if (g_shell_pid > 0) {
		sys_kill(g_shell_pid, SIGTERM);
		(void)sys_waitpid(g_shell_pid, 0);
		g_shell_pid = -1;
	}
	if (g_ptmx >= 0) {
		sys_close(g_ptmx);
		g_ptmx = -1;
	}
}

int main(void)
{
	drwin_window_t win;
	drwin_surface_t surface;

	if (drwin_connect() != 0)
		return 1;
	if (drwin_create_window("Terminal", 64, 64, TERM_W, TERM_H, &win) != 0)
		return 1;
	if (drwin_map_surface(win, &surface) != 0) {
		drwin_destroy_window(win);
		return 1;
	}
	term_clear();
	render_terminal(win, &surface);
	if (start_terminal_session() != 0) {
		drwin_destroy_window(win);
		return 1;
	}

	for (;;) {
		sys_pollfd_t pfds[2];
		int wm_fd = drwin_fd_for_poll();

		pfds[0].fd = g_ptmx;
		pfds[0].events = SYS_POLLIN;
		pfds[0].revents = 0;
		pfds[1].fd = wm_fd;
		pfds[1].events = SYS_POLLIN;
		pfds[1].revents = 0;
		if (sys_poll(pfds, 2, -1) <= 0)
			continue;
		if ((pfds[0].revents & SYS_POLLIN) &&
		    drain_pty_and_present(win, &surface) != 0)
			break;
		if ((pfds[1].revents & SYS_POLLIN) && drain_window_events() != 0)
			break;
	}

	stop_terminal_session();
	drwin_destroy_window(win);
	return 0;
}

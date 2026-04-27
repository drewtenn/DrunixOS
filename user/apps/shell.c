/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * shell.c — interactive ring-3 shell for the OS.
 */

#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/syscall.h"

#define KEY_PAGE_UP '\x01'   /* Legacy keyboard-driver scroll shortcut. */
#define KEY_PAGE_DOWN '\x02' /* Legacy keyboard-driver scroll shortcut. */
#define SHELL_HISTORY_MAX 16
#define SHELL_HISTORY_LINE 128

/* ANSI Color Codes */
#define TERM_COLOR_CYAN "\x1b[36m"
#define TERM_COLOR_GREEN "\x1b[32m"
#define TERM_COLOR_YELLOW "\x1b[33m"
#define TERM_COLOR_RED "\x1b[31m"
#define TERM_COLOR_RESET "\x1b[0m"

#define SHELL_MAX_ARGS 16

typedef struct {
	char *stdin_path;
	char *stdout_path;
	int stdout_append;
} shell_redir_t;

typedef struct {
	int saved_stdin;
	int saved_stdout;
} shell_redir_state_t;

static int tokenize(char *line, char **out_argv, int max_argv);
static int readline(char *buf, int max);
static void print_prompt(void);
static void redraw_prompt_line(const char *buf, int len, int prev_len);
static void shell_history_add(const char *line);
static void tab_complete(char *buf, int *n, int max);

static volatile int g_prompt_signal = 0;
static char g_history[SHELL_HISTORY_MAX][SHELL_HISTORY_LINE];
static int g_history_count = 0;
static int g_history_next = 0;

static void shell_prompt_signal(int sig)
{
	g_prompt_signal = sig;
}

static void shell_install_signal_state(void)
{
	sys_sigaction(SIGINT, shell_prompt_signal, 0);
	sys_sigaction(SIGTSTP, shell_prompt_signal, 0);
}

static void shell_reset_job_signal_state(void)
{
	sys_sigaction(SIGINT, SIG_DFL, 0);
	sys_sigaction(SIGTSTP, SIG_DFL, 0);
}

static int shell_pgid(void)
{
	return sys_getpgid(0);
}

static void shell_claim_foreground_tty(void)
{
	int pgid = shell_pgid();
	if (pgid > 0)
		sys_tcsetpgrp(0, pgid);
}

static const char *signal_name(int sig)
{
	switch (sig) {
	case SIGILL:
		return "Illegal instruction";
	case SIGTRAP:
		return "Trace/breakpoint trap";
	case SIGABRT:
		return "Aborted";
	case SIGFPE:
		return "Floating point exception";
	case SIGSEGV:
		return "Segmentation fault";
	case SIGPIPE:
		return "Broken pipe";
	case SIGINT:
		return "Interrupted";
	case SIGTERM:
		return "Terminated";
	default:
		return "Terminated";
	}
}

static void print_signal_status(int status)
{
	if (!WIFSIGNALED(status))
		return;
	printf(TERM_COLOR_RED "%s%s\n" TERM_COLOR_RESET,
	       signal_name(WTERMSIG(status)),
	       WCOREDUMP(status) ? " (core dumped)" : "");
}

/* ── built-in: ls ───────────────────────────────────────────────────────── */

/*
 * Build the path to pass to sys_stat for an entry returned by sys_getdents.
 * dir is the directory that was listed (NULL = root).
 * entry is the raw name from getdents (may have trailing '/').
 * out receives the stat path (no trailing slash).
 */
static void
ls_stat_path(const char *dir, const char *entry, char *out, int outsz)
{
	/* Copy entry, stripping any trailing '/'. */
	int elen = 0;
	while (entry[elen] && elen < outsz - 1) {
		out[elen] = entry[elen];
		elen++;
	}
	out[elen] = '\0';
	if (elen > 0 && out[elen - 1] == '/')
		out[--elen] = '\0';

	/* If a parent dir was given, prepend it. */
	if (dir && dir[0] != '\0') {
		/* shift entry right to make room for "dir/" prefix */
		int dlen = 0;
		while (dir[dlen])
			dlen++;
		int needed = dlen + 1 + elen; /* dir + '/' + entry */
		if (needed >= outsz)
			needed = outsz - 1;
		/* move existing entry to the right */
		for (int i = needed; i > dlen; i--)
			out[i] = out[i - dlen - 1];
		/* write dir/ prefix */
		for (int i = 0; i < dlen; i++)
			out[i] = dir[i];
		out[dlen] = '/';
		out[needed] = '\0';
	}
}

static int is_leap_year(unsigned int year)
{
	return ((year % 4u) == 0u && (year % 100u) != 0u) || ((year % 400u) == 0u);
}

static void format_mtime(unsigned int epoch, char *out, int outsz)
{
	static const char *months[] = {"Jan",
	                               "Feb",
	                               "Mar",
	                               "Apr",
	                               "May",
	                               "Jun",
	                               "Jul",
	                               "Aug",
	                               "Sep",
	                               "Oct",
	                               "Nov",
	                               "Dec"};
	static const unsigned int month_days[] = {
	    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

	unsigned int days = epoch / 86400u;
	unsigned int year = 1970u;
	while (1) {
		unsigned int ydays = is_leap_year(year) ? 366u : 365u;
		if (days < ydays)
			break;
		days -= ydays;
		year++;
	}

	unsigned int month = 0;
	while (month < 12) {
		unsigned int mdays = month_days[month];
		if (month == 1 && is_leap_year(year))
			mdays++;
		if (days < mdays)
			break;
		days -= mdays;
		month++;
	}

	unsigned int day = days + 1u;
	if (month >= 12)
		month = 11;
	snprintf(out, outsz, "%s %2u %4u", months[month], day, year);
}

#define LS_OUTPUT_BUF_SIZE 2048
#define LS_LINE_BUF_SIZE 512

static void ls_flush_output(char *out, int *len)
{
	if (!out || !len || *len <= 0)
		return;
	fwrite(out, 1, (size_t)*len, stdout);
	*len = 0;
}

static void ls_emit_output(char *out, int *len, const char *text, int text_len)
{
	if (!out || !len || !text || text_len <= 0)
		return;
	if (text_len >= LS_OUTPUT_BUF_SIZE) {
		ls_flush_output(out, len);
		fwrite(text, 1, (size_t)text_len, stdout);
		return;
	}
	if (*len + text_len > LS_OUTPUT_BUF_SIZE)
		ls_flush_output(out, len);
	memcpy(out + *len, text, (size_t)text_len);
	*len += text_len;
}

static void ls_emit_line(char *out, int *out_len, const char *line, int len)
{
	if (len < 0)
		len = 0;
	if (len >= LS_LINE_BUF_SIZE)
		len = LS_LINE_BUF_SIZE - 1;
	ls_emit_output(out, out_len, line, len);
}

static void cmd_ls(const char *path)
{
	/* Strip a trailing '/' so "ls tests/" and "ls tests" both work. */
	char pathbuf[4096];
	const char *arg = 0;
	if (path) {
		int i = 0;
		while (i < 4095 && path[i]) {
			pathbuf[i] = path[i];
			i++;
		}
		pathbuf[i] = '\0';
		/* Strip trailing slash. */
		if (i > 0 && pathbuf[i - 1] == '/')
			pathbuf[--i] = '\0';
		arg = pathbuf[0] ? pathbuf : 0;
	}
	/* If no path: arg stays NULL → kernel uses process cwd for getdents. */

	char buf[512];
	char out[LS_OUTPUT_BUF_SIZE];
	int out_len = 0;
	int n = sys_getdents(arg, buf, sizeof(buf));
	int i = 0;
	while (i < n) {
		char *name = buf + i;
		if (*name == '\0') {
			i++;
			continue;
		}

		/* Determine type from trailing '/' (set by getdents for directories). */
		int nlen = 0;
		while (name[nlen])
			nlen++;
		int is_dir = (nlen > 0 && name[nlen - 1] == '/');

		/* Try to get precise metadata via sys_stat. */
		dufs_stat_t st;
		char spath[4096];
		ls_stat_path(arg, name, spath, sizeof(spath));
		int have_stat = (sys_stat(spath, &st) == 0);

		if (have_stat) {
			is_dir = (st.type == 2);
			unsigned int links = st.link_count ? st.link_count : 1;
			char mtime[16];
			char line[LS_LINE_BUF_SIZE];
			int line_len;

			format_mtime(st.mtime, mtime, sizeof(mtime));
			line_len = snprintf(line,
			                    sizeof(line),
			                    "%s%s  %u root root %6u %s %s\n",
			                    is_dir ? "d" : "-",
			                    is_dir ? "rwxr-xr-x" : "rw-r--r--",
			                    links,
			                    st.size,
			                    mtime,
			                    name);
			ls_emit_line(out, &out_len, line, line_len);
		} else {
			char line[LS_LINE_BUF_SIZE];
			int line_len;

			/* stat failed — print bare name */
			line_len = snprintf(line, sizeof(line), "%s\n", name);
			ls_emit_line(out, &out_len, line, line_len);
		}

		while (i < n && buf[i] != '\0')
			i++;
		i++;
	}
	ls_flush_output(out, &out_len);
}

/* ── built-in: cd ───────────────────────────────────────────────────────── */

static void cmd_cd(const char *dir)
{
	/*
     * Delegate all path validation and cwd mutation to the kernel.
     * SYS_CHDIR handles NULL/""/"/", "..", absolute paths, and relative paths.
     */
	if (sys_chdir(dir) != 0)
		printf("cd: no such directory: %s\n", dir ? dir : "/");
}

/* ── minimal shell built-ins ────────────────────────────────────────────── */

#define MAX_ENV_VARS 32
#define ENV_NAME_MAX 32
#define ENV_VALUE_MAX 96
#define ENV_ENTRY_MAX (ENV_NAME_MAX + ENV_VALUE_MAX + 1)

typedef struct {
	int used;
	int exported;
	int readonly;
	char name[ENV_NAME_MAX];
	char value[ENV_VALUE_MAX];
} shell_env_t;

static shell_env_t shell_env[MAX_ENV_VARS];
static char shell_env_entries[MAX_ENV_VARS][ENV_ENTRY_MAX];
static char *shell_envp[MAX_ENV_VARS + 1];
static int last_status = 0;

static int env_name_ok(const char *name, int len)
{
	if (!name || len <= 0)
		return 0;
	if (len >= ENV_NAME_MAX)
		return 0;
	if (!((name[0] >= 'A' && name[0] <= 'Z') ||
	      (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
		return 0;
	for (int i = 1; i < len; i++) {
		char c = name[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '_'))
			return 0;
	}
	return 1;
}

static void env_rebuild_environ(void)
{
	int out = 0;
	for (int i = 0; i < MAX_ENV_VARS; i++) {
		if (!shell_env[i].used)
			continue;
		if (!shell_env[i].exported)
			continue;
		snprintf(shell_env_entries[out],
		         ENV_ENTRY_MAX,
		         "%s=%s",
		         shell_env[i].name,
		         shell_env[i].value);
		shell_envp[out] = shell_env_entries[out];
		out++;
	}
	shell_envp[out] = 0;
	environ = shell_envp;
}

static int env_compose_exec(char **envp_out, int max_envp)
{
	int out = 0;

	if (!envp_out || max_envp <= 0)
		return 0;

	env_rebuild_environ();

	for (int i = 0; i < MAX_ENV_VARS && out < max_envp - 1; i++) {
		if (!shell_env[i].used || !shell_env[i].exported)
			continue;
		envp_out[out] = shell_env_entries[out];
		out++;
	}

	envp_out[out] = 0;
	return out;
}

static int env_find(const char *name, int len)
{
	for (int i = 0; i < MAX_ENV_VARS; i++) {
		if (!shell_env[i].used)
			continue;
		if ((int)strlen(shell_env[i].name) == len &&
		    strncmp(shell_env[i].name, name, (size_t)len) == 0)
			return i;
	}
	return -1;
}

static int env_slot(void)
{
	for (int i = 0; i < MAX_ENV_VARS; i++)
		if (!shell_env[i].used)
			return i;
	return -1;
}

static const char *env_get_value(const char *name)
{
	int idx = env_find(name, (int)strlen(name));
	return idx >= 0 ? shell_env[idx].value : 0;
}

static int env_set_value(const char *name,
                         int name_len,
                         const char *value,
                         int exported,
                         int readonly)
{
	int idx;

	if (!env_name_ok(name, name_len))
		return -1;

	idx = env_find(name, name_len);
	if (idx >= 0 && shell_env[idx].readonly)
		return -2;
	if (idx < 0)
		idx = env_slot();
	if (idx < 0)
		return -3;

	shell_env[idx].used = 1;
	if (exported)
		shell_env[idx].exported = 1;
	if (readonly)
		shell_env[idx].readonly = 1;
	strncpy(shell_env[idx].name, name, (size_t)name_len);
	shell_env[idx].name[name_len] = '\0';
	strncpy(shell_env[idx].value, value ? value : "", ENV_VALUE_MAX - 1);
	shell_env[idx].value[ENV_VALUE_MAX - 1] = '\0';
	env_rebuild_environ();
	return 0;
}

static void env_import(char **envp)
{
	if (!envp)
		return;

	for (int i = 0; envp[i]; i++) {
		char *eq = strchr(envp[i], '=');
		if (!eq)
			continue;

		int name_len = (int)(eq - envp[i]);
		if (!env_name_ok(envp[i], name_len))
			continue;

		env_set_value(envp[i], name_len, eq + 1, 1, 0);
	}
}

static void cmd_pwd(void)
{
	char cwd_buf[64];
	if (sys_getcwd(cwd_buf, sizeof(cwd_buf)) < 0) {
		printf("pwd: cannot read current directory\n");
		return;
	}

	printf("%s\n", cwd_buf);
}

static void cmd_echo(int argc, char **argv)
{
	int start = 1;
	int newline = 1;

	if (argc > 1 && strcmp(argv[1], "-n") == 0) {
		newline = 0;
		start = 2;
	}

	for (int i = start; i < argc; i++) {
		if (i > start)
			putchar(' ');
		printf("%s", argv[i]);
	}
	if (newline)
		putchar('\n');
}

static void cmd_export(int argc, char **argv)
{
	if (argc == 1) {
		for (int i = 0; i < MAX_ENV_VARS; i++)
			if (shell_env[i].used && shell_env[i].exported)
				printf("export %s=%s\n", shell_env[i].name, shell_env[i].value);
		return;
	}

	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		int name_len = eq ? (int)(eq - argv[i]) : (int)strlen(argv[i]);
		const char *value = eq ? eq + 1 : "";
		int rc;

		if (!env_name_ok(argv[i], name_len)) {
			printf("export: invalid name: %s\n", argv[i]);
			continue;
		}

		if (!eq) {
			int idx = env_find(argv[i], name_len);
			if (idx < 0)
				rc = env_set_value(argv[i], name_len, "", 1, 0);
			else {
				shell_env[idx].exported = 1;
				env_rebuild_environ();
				rc = 0;
			}
		} else {
			rc = env_set_value(argv[i], name_len, value, 1, 0);
		}

		if (rc == -2)
			printf("export: readonly variable: %s\n", argv[i]);
		else if (rc == -3)
			printf("export: environment full\n");
	}
}

static void cmd_unset(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: unset <name> [name...]\n");
		return;
	}

	for (int i = 1; i < argc; i++) {
		int len = (int)strlen(argv[i]);
		if (!env_name_ok(argv[i], len)) {
			printf("unset: invalid name: %s\n", argv[i]);
			continue;
		}

		int idx = env_find(argv[i], len);
		if (idx >= 0) {
			if (shell_env[idx].readonly) {
				printf("unset: readonly variable: %s\n", argv[i]);
				continue;
			}
			shell_env[idx].used = 0;
			shell_env[idx].exported = 0;
			shell_env[idx].readonly = 0;
			shell_env[idx].name[0] = '\0';
			shell_env[idx].value[0] = '\0';
		}
	}
	env_rebuild_environ();
}

static void cmd_read(int argc, char **argv)
{
	char line[128];
	char *names[SHELL_MAX_ARGS];
	int name_count = 0;

	if (argc == 1)
		names[name_count++] = (char *)"REPLY";
	else {
		for (int i = 1; i < argc && name_count < SHELL_MAX_ARGS; i++) {
			int len = (int)strlen(argv[i]);
			if (!env_name_ok(argv[i], len)) {
				printf("read: invalid name: %s\n", argv[i]);
				return;
			}
			names[name_count++] = argv[i];
		}
	}

	if (readline(line, sizeof(line)) < 0)
		return;

	if (name_count == 1) {
		int rc = env_set_value(names[0], (int)strlen(names[0]), line, 0, 0);
		if (rc == -2)
			printf("read: readonly variable: %s\n", names[0]);
		else if (rc == -3)
			printf("read: variable table full\n");
		return;
	}

	char *parts[SHELL_MAX_ARGS];
	int pc = tokenize(line, parts, SHELL_MAX_ARGS);
	for (int i = 0; i < name_count; i++) {
		const char *value = (i < pc) ? parts[i] : "";
		int rc = env_set_value(names[i], (int)strlen(names[i]), value, 0, 0);
		if (rc == -2)
			printf("read: readonly variable: %s\n", names[i]);
		else if (rc == -3)
			printf("read: variable table full\n");
	}
}

static void cmd_readonly(int argc, char **argv)
{
	if (argc == 1) {
		for (int i = 0; i < MAX_ENV_VARS; i++)
			if (shell_env[i].used && shell_env[i].readonly)
				printf(
				    "readonly %s=%s\n", shell_env[i].name, shell_env[i].value);
		return;
	}

	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		int name_len = eq ? (int)(eq - argv[i]) : (int)strlen(argv[i]);
		const char *value = eq ? eq + 1 : "";
		int rc;

		if (!env_name_ok(argv[i], name_len)) {
			printf("readonly: invalid name: %s\n", argv[i]);
			continue;
		}
		if (!eq) {
			int idx = env_find(argv[i], name_len);
			if (idx >= 0) {
				shell_env[idx].readonly = 1;
				rc = 0;
			} else
				rc = env_set_value(argv[i], name_len, "", 0, 1);
		} else
			rc = env_set_value(argv[i], name_len, value, 0, 1);

		if (rc == -2)
			printf("readonly: readonly variable: %s\n", argv[i]);
		else if (rc == -3)
			printf("readonly: variable table full\n");
	}
}

static int parse_exit_status(const char *s, int *out)
{
	int sign = 1;
	int n = 0;

	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}

	if (*s == '\0')
		return 0;
	while (*s) {
		if (*s < '0' || *s > '9')
			return 0;
		n = n * 10 + (*s - '0');
		s++;
	}

	*out = sign * n;
	return 1;
}

/* ── built-in: cat ──────────────────────────────────────────────────────── */

static void cmd_cat_fd(int fd)
{
	/* Use the raw binary-safe path here rather than printf("%s"): file
     * contents may contain embedded NUL bytes (e.g. when a user `cat`s a
     * compiled ELF), and the counted-byte sys_write_n preserves them. */
	char buf[512];
	for (;;) {
		int n = sys_read(fd, buf, (int)sizeof(buf));
		if (n <= 0)
			break;
		sys_write_n(buf, n);
	}
}

static void cmd_cat(const char *name)
{
	int fd = sys_open(name);
	if (fd < 0) {
		printf("cat: not found: %s\n", name);
		return;
	}

	cmd_cat_fd(fd);
	sys_close(fd);
}

/* ── built-in: mkdir ────────────────────────────────────────────────────── */

static void cmd_mkdir(const char *name)
{
	if (sys_mkdir(name) != 0)
		printf("mkdir: failed: %s\n", name);
}

/* ── built-in: rmdir ────────────────────────────────────────────────────── */

static void cmd_rmdir(const char *name)
{
	if (sys_rmdir(name) != 0)
		printf("rmdir: failed (not found, not empty, or error): %s\n", name);
}

/* ── built-in: rm ───────────────────────────────────────────────────────── */

static void cmd_rm(const char *name)
{
	if (sys_unlink(name) != 0)
		printf("rm: cannot remove: %s\n", name);
}

/* ── path helpers ────────────────────────────────────────────────────────── */

/* Return a pointer to the last component of path (never returns NULL). */
static const char *path_basename(const char *path)
{
	const char *last = path;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			last = p + 1;
	return last;
}

/* Write dir + '/' + name into out[outsz]. */
static void path_join(const char *dir, const char *name, char *out, int outsz)
{
	int i = 0, j = 0;
	while (i < outsz - 2 && dir[j])
		out[i++] = dir[j++];
	if (i < outsz - 1)
		out[i++] = '/';
	j = 0;
	while (i < outsz - 1 && name[j])
		out[i++] = name[j++];
	out[i] = '\0';
}

/* Strip one trailing '/' in place. */
static void strip_slash(char *path)
{
	int i = 0;
	while (path[i])
		i++;
	if (i > 0 && path[i - 1] == '/')
		path[i - 1] = '\0';
}

/* Returns 1 if path names an existing directory. */
static int path_is_dir(const char *path)
{
	char buf[8];
	return sys_getdents(path, buf, sizeof(buf)) >= 0;
}

/* Returns 1 if path names an existing regular file. */
static int path_is_file(const char *path)
{
	int fd = sys_open(path);
	if (fd >= 0) {
		sys_close(fd);
		return 1;
	}
	return 0;
}

static int path_has_slash(const char *path)
{
	for (const char *p = path; *p; p++)
		if (*p == '/')
			return 1;
	return 0;
}

static int build_command_path(
    const char *dir, int dir_len, const char *name, char *out, int outsz)
{
	int i = 0;

	if (outsz <= 0)
		return -1;

	if (dir_len == 0) {
		while (i < outsz - 1 && name[i]) {
			out[i] = name[i];
			i++;
		}
		out[i] = '\0';
		return name[i] ? -1 : 0;
	}

	for (int j = 0; j < dir_len && i < outsz - 1; j++)
		out[i++] = dir[j];
	if (i > 0 && out[i - 1] != '/' && i < outsz - 1)
		out[i++] = '/';
	for (int j = 0; name[j] && i < outsz - 1; j++)
		out[i++] = name[j];
	out[i] = '\0';
	return 0;
}

static int resolve_command_path(const char *name, char *out, int outsz)
{
	const char *path;
	const char *start;

	if (!name || !*name)
		return -1;
	if (outsz <= 0)
		return -1;

	if (path_has_slash(name)) {
		strncpy(out, name, (size_t)outsz - 1);
		out[outsz - 1] = '\0';
		return path_is_file(out) ? 0 : -1;
	}

	path = env_get_value("PATH");
	if (!path || !*path)
		return -1;

	start = path;
	for (;;) {
		const char *end = start;
		char candidate[128];
		int len;

		while (*end && *end != ':')
			end++;
		len = (int)(end - start);

		if (build_command_path(
		        start, len, name, candidate, sizeof(candidate)) == 0 &&
		    path_is_file(candidate)) {
			strncpy(out, candidate, (size_t)outsz - 1);
			out[outsz - 1] = '\0';
			return 0;
		}

		if (!*end)
			break;
		start = end + 1;
	}

	return -1;
}

/* ── built-in: cp ───────────────────────────────────────────────────────── */

/*
 * Copy the file at rsrc to rdst (both fully-resolved paths).
 * Returns 0 on success, -1 if src cannot be opened, -2 if dst cannot be created.
 */
static int do_copy_file(const char *rsrc, const char *rdst)
{
	int rfd = sys_open(rsrc);
	if (rfd < 0)
		return -1;

	int wfd = sys_create(rdst);
	if (wfd < 0) {
		sys_close(rfd);
		return -2;
	}

	char buf[512];
	for (;;) {
		int n = sys_read(rfd, buf, (int)sizeof(buf));
		if (n <= 0)
			break;
		sys_fwrite(wfd, buf, n);
	}
	sys_close(rfd);
	sys_close(wfd);
	return 0;
}

static void cmd_cp(const char *src, const char *dst)
{
	char rsrc[40], rdst[40];
	strncpy(rsrc, src, sizeof(rsrc) - 1);
	rsrc[sizeof(rsrc) - 1] = '\0';
	strncpy(rdst, dst, sizeof(rdst) - 1);
	rdst[sizeof(rdst) - 1] = '\0';
	strip_slash(rsrc);
	strip_slash(rdst);

	/* cp does not recurse into directories (like cp without -r on Linux). */
	if (path_is_dir(rsrc)) {
		printf("cp: omitting directory: %s\n", rsrc);
		return;
	}

	/* Determine effective destination path. */
	char edst[40];
	if (path_is_dir(rdst)) {
		path_join(rdst, path_basename(rsrc), edst, sizeof(edst));
	} else {
		strncpy(edst, rdst, sizeof(edst) - 1);
		edst[sizeof(edst) - 1] = '\0';
	}

	if (do_copy_file(rsrc, edst) < 0)
		printf("cp: %s: no such file\n", rsrc);
}

/* ── built-in: mv ───────────────────────────────────────────────────────── */

static void cmd_mv(const char *src, const char *dst)
{
	char rsrc[40], rdst[40];
	strncpy(rsrc, src, sizeof(rsrc) - 1);
	rsrc[sizeof(rsrc) - 1] = '\0';
	strncpy(rdst, dst, sizeof(rdst) - 1);
	rdst[sizeof(rdst) - 1] = '\0';
	strip_slash(rsrc);
	strip_slash(rdst);

	int src_is_dir = path_is_dir(rsrc);
	int dst_is_dir = path_is_dir(rdst);

	if (!src_is_dir && !path_is_file(rsrc)) {
		printf("mv: %s: no such file or directory\n", rsrc);
		return;
	}

	if (src_is_dir && dst_is_dir) {
		printf("mv: cannot move directory into existing directory\n");
		return;
	}

	/* Determine effective destination for file-into-directory case. */
	char edst[40];
	if (!src_is_dir && dst_is_dir) {
		path_join(rdst, path_basename(rsrc), edst, sizeof(edst));
	} else {
		strncpy(edst, rdst, sizeof(edst) - 1);
		edst[sizeof(edst) - 1] = '\0';
	}

	if (sys_rename(rsrc, edst) != 0)
		printf("mv: rename failed: %s\n", rsrc);
}

/* ── built-in: help ─────────────────────────────────────────────────────── */

static void cmd_help(void)
{
	printf("Built-in commands:\n"
	       "  help            show this message\n"
	       "  exit [status]   exit the shell\n"
	       "  pwd             print current directory\n"
	       "  echo [-n] ...   print arguments\n"
	       "  export [N=V]    set/list exported environment variables\n"
	       "  readonly [N=V]  set/list readonly shell variables\n"
	       "  read [name...]  read one input line into shell variables\n"
	       "  unset <name>    remove exported environment variables\n"
	       "  true, false, :  POSIX status built-ins\n"
	       "  test, [         evaluate simple test expressions\n"
	       "  type <name>     describe command names\n"
	       "  command -v name locate command names\n"
	       "  kill ...        send a signal to a process/group\n"
	       "  wait [job...]   wait for shell jobs\n"
	       "  ls [dir]        list files (optionally in a subdirectory)\n"
	       "  cd [dir]        change directory (no arg = go to root)\n"
	       "  mkdir <dir>     create a directory\n"
	       "  rmdir <dir>     remove an empty directory\n"
	       "  cat <file>      print a file to the screen\n"
	       "  rm <file>       delete a file\n"
	       "  cp <src> <dst>  copy a file\n"
	       "  mv <src> <dst>  move (rename) a file\n"
	       "  clear           clear the screen\n"
	       "  exec <cmd> ...  replace the shell with a program\n"
	       "  modload <file>  load a kernel module\n"
	       "  jobs            list stopped/background jobs\n"
	       "  fg [N]          resume job N in the foreground\n"
	       "  bg [N]          resume stopped job N in the background\n"
	       "  cmd > file      redirect stdout to a file\n"
	       "  cmd >> file     append stdout to a file\n"
	       "  cmd < file      redirect stdin from a file\n"
	       "  Ctrl+Z          stop the foreground program\n"
	       "  Page Up/Down    scroll through terminal history\n"
	       "Any other input tries to run a program by that name.\n");
}

/* ── in-place tokenizer ─────────────────────────────────────────────────── */

/*
 * Split `line` on runs of spaces.  Overwrites spaces with NULs so each
 * token is a valid C string, and fills out_argv[] with pointers into
 * `line`.  Stops when out_argv is full (returns at most max_argv-1 tokens
 * and leaves the NULL terminator slot) or when the input is exhausted.
 *
 * Returns the token count.  out_argv[count] is set to NULL.
 */
static int tokenize(char *line, char **out_argv, int max_argv)
{
	int argc = 0;
	char *p = line;

	while (*p && argc < max_argv - 1) {
		while (*p == ' ')
			p++; /* eat leading whitespace */
		if (!*p)
			break;

		out_argv[argc++] = p; /* start of a token */
		while (*p && *p != ' ')
			p++; /* scan to end of token */
		if (*p) {
			*p = '\0'; /* terminate this token in place */
			p++;
		}
	}
	out_argv[argc] = (char *)0;
	return argc;
}

static void shell_redir_init(shell_redir_t *redir)
{
	redir->stdin_path = 0;
	redir->stdout_path = 0;
	redir->stdout_append = 0;
}

static int shell_redir_token(const char *token)
{
	return !strcmp(token, "<") || !strcmp(token, ">") || !strcmp(token, ">>");
}

static int
shell_parse_redirections(char **argv, int *argc, shell_redir_t *redir)
{
	int out = 0;

	shell_redir_init(redir);
	for (int i = 0; i < *argc; i++) {
		if (!shell_redir_token(argv[i])) {
			argv[out++] = argv[i];
			continue;
		}

		if (i + 1 >= *argc || shell_redir_token(argv[i + 1])) {
			printf("redirection: missing path after %s\n", argv[i]);
			return -1;
		}

		if (!strcmp(argv[i], "<")) {
			redir->stdin_path = argv[i + 1];
		} else {
			redir->stdout_path = argv[i + 1];
			redir->stdout_append = !strcmp(argv[i], ">>");
		}
		i++;
	}

	argv[out] = (char *)0;
	*argc = out;
	if (out == 0) {
		printf("redirection: missing command\n");
		return -1;
	}
	return 0;
}

static int shell_open_output_redir(const shell_redir_t *redir)
{
	int fd;

	if (!redir->stdout_append)
		return sys_create(redir->stdout_path);

	fd = sys_open_flags(redir->stdout_path, SYS_O_WRONLY | SYS_O_APPEND, 0666);
	if (fd >= 0)
		return fd;
	return sys_create(redir->stdout_path);
}

static int shell_redir_apply(const shell_redir_t *redir)
{
	if (redir->stdin_path) {
		int fd = sys_open(redir->stdin_path);
		if (fd < 0) {
			printf("redirection: cannot open input: %s\n", redir->stdin_path);
			return -1;
		}
		if (sys_dup2(fd, 0) < 0) {
			sys_close(fd);
			printf("redirection: cannot redirect input\n");
			return -1;
		}
		sys_close(fd);
	}

	if (redir->stdout_path) {
		int fd = shell_open_output_redir(redir);
		if (fd < 0) {
			printf("redirection: cannot open output: %s\n", redir->stdout_path);
			return -1;
		}
		if (sys_dup2(fd, 1) < 0) {
			sys_close(fd);
			printf("redirection: cannot redirect output\n");
			return -1;
		}
		sys_close(fd);
	}

	return 0;
}

static int shell_redir_begin(const shell_redir_t *redir,
                             shell_redir_state_t *state)
{
	state->saved_stdin = -1;
	state->saved_stdout = -1;

	if (redir->stdin_path) {
		state->saved_stdin = sys_dup(0);
		if (state->saved_stdin < 0) {
			printf("redirection: cannot save stdin\n");
			return -1;
		}
	}

	if (redir->stdout_path) {
		state->saved_stdout = sys_dup(1);
		if (state->saved_stdout < 0) {
			if (state->saved_stdin >= 0)
				sys_close(state->saved_stdin);
			state->saved_stdin = -1;
			printf("redirection: cannot save stdout\n");
			return -1;
		}
	}

	if (shell_redir_apply(redir) != 0) {
		if (state->saved_stdin >= 0) {
			sys_dup2(state->saved_stdin, 0);
			sys_close(state->saved_stdin);
			state->saved_stdin = -1;
		}
		if (state->saved_stdout >= 0) {
			sys_dup2(state->saved_stdout, 1);
			sys_close(state->saved_stdout);
			state->saved_stdout = -1;
		}
		return -1;
	}

	return 0;
}

static void shell_redir_end(shell_redir_state_t *state)
{
	if (state->saved_stdin >= 0) {
		sys_dup2(state->saved_stdin, 0);
		sys_close(state->saved_stdin);
		state->saved_stdin = -1;
	}
	if (state->saved_stdout >= 0) {
		sys_dup2(state->saved_stdout, 1);
		sys_close(state->saved_stdout);
		state->saved_stdout = -1;
	}
}

/* ── readline ───────────────────────────────────────────────────────────── */

static int read_char(void)
{
	char c;
	int n = sys_read(0, &c, 1);
	if (n == 1)
		return (unsigned char)c;
	return -1;
}

static int shell_history_slot(int logical_index)
{
	if (g_history_count < SHELL_HISTORY_MAX)
		return logical_index;
	return (g_history_next + logical_index) % SHELL_HISTORY_MAX;
}

static void shell_history_add(const char *line)
{
	int slot;

	if (!line || !*line)
		return;
	if (g_history_count > 0) {
		int last = shell_history_slot(g_history_count - 1);
		if (strcmp(g_history[last], line) == 0)
			return;
	}

	slot = g_history_next;
	strncpy(g_history[slot], line, SHELL_HISTORY_LINE - 1);
	g_history[slot][SHELL_HISTORY_LINE - 1] = '\0';
	g_history_next = (g_history_next + 1) % SHELL_HISTORY_MAX;
	if (g_history_count < SHELL_HISTORY_MAX)
		g_history_count++;
}

static void readline_copy_text(char *buf, int *n, int max, const char *text)
{
	int old_n = *n;

	strncpy(buf, text, (size_t)max - 1u);
	buf[max - 1] = '\0';
	*n = (int)strlen(buf);
	redraw_prompt_line(buf, *n, old_n);
}

static void readline_recall_history(char *buf,
                                    int *n,
                                    int max,
                                    int *history_pos,
                                    char *draft)
{
	if (g_history_count == 0)
		return;
	if (*history_pos == g_history_count) {
		strncpy(draft, buf, SHELL_HISTORY_LINE - 1);
		draft[SHELL_HISTORY_LINE - 1] = '\0';
	}
	if (*history_pos > 0)
		(*history_pos)--;
	readline_copy_text(
	    buf, n, max, g_history[shell_history_slot(*history_pos)]);
}

static void readline_forward_history(char *buf,
                                     int *n,
                                     int max,
                                     int *history_pos,
                                     const char *draft)
{
	if (g_history_count == 0 || *history_pos >= g_history_count)
		return;
	(*history_pos)++;
	if (*history_pos == g_history_count)
		readline_copy_text(buf, n, max, draft);
	else
		readline_copy_text(
		    buf, n, max, g_history[shell_history_slot(*history_pos)]);
}

static void
readline_handle_escape(char *buf, int *n, int max, int *history_pos, char *draft)
{
	int c = read_char();

	if (c != '[' && c != 'O')
		return;

	c = read_char();
	if (c < 0)
		return;

	if (c >= '0' && c <= '9') {
		int final = read_char();
		if (final != '~')
			return;
		if (c == '5')
			sys_scroll_up(5);
		else if (c == '6')
			sys_scroll_down(5);
		return;
	}

	if (c == 'A') {
		readline_recall_history(buf, n, max, history_pos, draft);
		return;
	}
	if (c == 'B') {
		readline_forward_history(buf, n, max, history_pos, draft);
		return;
	}
}

static int readline(char *buf, int max)
{
	int n = 0;
	int history_pos = g_history_count;
	char draft[SHELL_HISTORY_LINE];

	draft[0] = '\0';
	while (1) {
		int rc = read_char();
		if (rc < 0) {
			if (g_prompt_signal == SIGINT || g_prompt_signal == SIGTSTP)
				g_prompt_signal = 0;
			buf[0] = '\0';
			return 0;
		}
		char c = (char)rc;
		if (c == '\r' || c == '\n') {
			putchar('\n');
			break;
		}
		if (c == KEY_PAGE_UP) {
			sys_scroll_up(5);
			continue;
		}
		if (c == KEY_PAGE_DOWN) {
			sys_scroll_down(5);
			continue;
		}
		if (c == '\x1b') {
			readline_handle_escape(buf, &n, max, &history_pos, draft);
			continue;
		}
		if (c == '\x03') {
			buf[0] = '\0';
			return 0;
		}
		if (c == '\b' || c == 127) {
			if (n > 0) {
				int old_n = n;
				n--;
				buf[n] = '\0';
				redraw_prompt_line(buf, n, old_n);
			}
			continue;
		}
		if (c == '\t') {
			buf[n] = '\0';
			tab_complete(buf, &n, max);
			continue;
		}
		if (n >= max - 1)
			continue;
		putchar(c); /* echo the character */
		buf[n++] = c;
		buf[n] = '\0';
	}
	buf[n] = '\0';
	return n;
}

static void print_prompt(void)
{
	char cwd_buf[64];
	sys_getcwd(cwd_buf, sizeof(cwd_buf));
	printf(TERM_COLOR_CYAN "drunix:" TERM_COLOR_GREEN "%s" TERM_COLOR_CYAN
	                       "> " TERM_COLOR_RESET,
	       cwd_buf);
}

static void redraw_prompt_line(const char *buf, int len, int prev_len)
{
	putchar('\r');
	print_prompt();
	for (int i = 0; i < len; i++)
		putchar(buf[i]);
	for (int i = len; i < prev_len; i++)
		putchar(' ');
	for (int i = len; i < prev_len; i++)
		putchar('\b');
}

/* ── tab completion ─────────────────────────────────────────────────────── */

#define TC_MAX_MATCHES 16
#define TC_MATCH_LEN 64

/*
 * Enumerate directory `dir` (NULL = process cwd) and collect entries whose
 * name starts with `pfx` into out[][TC_MATCH_LEN].  Returns the number of
 * matches (at most `max`).
 */
static int collect_path_matches(const char *dir,
                                const char *pfx,
                                char out[][TC_MATCH_LEN],
                                int max)
{
	char dents[2048];
	int pfx_len = (int)strlen(pfx);
	int n = sys_getdents(dir, dents, (int)sizeof(dents));
	if (n <= 0)
		return 0;

	int count = 0;
	int i = 0;
	while (i < n && count < max) {
		char *entry = dents + i;
		if (*entry == '\0') {
			i++;
			continue;
		}

		if (pfx_len == 0 || strncmp(entry, pfx, (size_t)pfx_len) == 0) {
			strncpy(out[count], entry, TC_MATCH_LEN - 1);
			out[count][TC_MATCH_LEN - 1] = '\0';
			count++;
		}

		while (i < n && dents[i] != '\0')
			i++;
		i++;
	}
	return count;
}

/*
 * Collect command-name completions for the first shell token.
 * Matches built-in names first, then executables found in $PATH directories.
 */
static int
collect_command_matches(const char *pfx, char out[][TC_MATCH_LEN], int max)
{
	static const char *all_builtins[] = {
	    ".",       ":",    "[",     "bg",       "cat",  "cd",     "clear",
	    "command", "cp",   "echo",  "exec",     "exit", "export", "false",
	    "fg",      "help", "jobs",  "kill",     "ls",   "mkdir",  "modload",
	    "mv",      "pwd",  "read",  "readonly", "rm",   "rmdir",  "test",
	    "true",    "type", "unset", "wait",     NULL};

	int pfx_len = (int)strlen(pfx);
	int count = 0;

	for (int i = 0; all_builtins[i] && count < max; i++) {
		if (pfx_len == 0 ||
		    strncmp(all_builtins[i], pfx, (size_t)pfx_len) == 0) {
			strncpy(out[count], all_builtins[i], TC_MATCH_LEN - 1);
			out[count][TC_MATCH_LEN - 1] = '\0';
			count++;
		}
	}

	/* Search each directory in $PATH */
	const char *path = env_get_value("PATH");
	if (!path || !*path)
		return count;

	const char *start = path;
	while (*start && count < max) {
		const char *end = start;
		while (*end && *end != ':')
			end++;

		int dir_len = (int)(end - start);
		if (dir_len > 0 && dir_len < TC_MATCH_LEN - 1) {
			char dir[TC_MATCH_LEN];
			strncpy(dir, start, (size_t)dir_len);
			dir[dir_len] = '\0';

			char tmp[TC_MAX_MATCHES][TC_MATCH_LEN];
			int want = max - count;
			if (want > TC_MAX_MATCHES)
				want = TC_MAX_MATCHES;
			int nm = collect_path_matches(dir, pfx, tmp, want);

			for (int j = 0; j < nm && count < max; j++) {
				/* Strip trailing '/' — executables don't need the directory marker */
				char name[TC_MATCH_LEN];
				strncpy(name, tmp[j], TC_MATCH_LEN - 1);
				name[TC_MATCH_LEN - 1] = '\0';
				int el = (int)strlen(name);
				if (el > 0 && name[el - 1] == '/')
					name[el - 1] = '\0';

				/* Skip duplicates already in out[] */
				int dup = 0;
				for (int k = 0; k < count; k++)
					if (strcmp(out[k], name) == 0) {
						dup = 1;
						break;
					}
				if (dup)
					continue;

				strncpy(out[count], name, TC_MATCH_LEN - 1);
				out[count][TC_MATCH_LEN - 1] = '\0';
				count++;
			}
		}

		if (!*end)
			break;
		start = end + 1;
	}

	return count;
}

/*
 * Return the length of the longest common prefix shared by all `count`
 * strings in `m`.
 */
static int common_prefix_len(char m[][TC_MATCH_LEN], int count)
{
	if (count == 0)
		return 0;
	int len = (int)strlen(m[0]);
	for (int i = 1; i < count; i++) {
		int j = 0;
		while (j < len && m[i][j] == m[0][j])
			j++;
		len = j;
	}
	return len;
}

/*
 * Called from readline() when a Tab character is received.  Performs
 * command completion (for the first token) or path completion (for
 * subsequent tokens), inserts the longest common prefix, and lists all
 * matches when there is ambiguity.
 */
static void tab_complete(char *buf, int *n, int max)
{
	/* Locate the start of the word being completed */
	int word_start = 0;
	for (int i = 0; i < *n; i++)
		if (buf[i] == ' ')
			word_start = i + 1;

	char *word = buf + word_start;
	int word_len = *n - word_start;
	int is_command = (word_start == 0);

	/* For path completion: find the last '/' to split dir from name prefix */
	int last_slash = -1;
	if (!is_command)
		for (int i = 0; i < word_len; i++)
			if (word[i] == '/')
				last_slash = i;

	/* name_start is the offset within `word` where the entry name begins */
	int name_start = (last_slash >= 0) ? (last_slash + 1) : 0;
	int name_pfx_len = word_len - name_start;

	char matches[TC_MAX_MATCHES][TC_MATCH_LEN];
	int mc;

	if (is_command) {
		mc = collect_command_matches(word, matches, TC_MAX_MATCHES);
	} else {
		/* Build directory string from word[0..last_slash] */
		char dir[TC_MATCH_LEN];
		if (last_slash < 0) {
			dir[0] = '\0';
		} else {
			/* Preserve the leading '/' for root paths */
			int dl = (last_slash == 0) ? 1 : last_slash;
			if (dl >= TC_MATCH_LEN)
				dl = TC_MATCH_LEN - 1;
			strncpy(dir, word, (size_t)dl);
			dir[dl] = '\0';
		}
		mc = collect_path_matches(
		    dir[0] ? dir : NULL, word + name_start, matches, TC_MAX_MATCHES);
	}

	if (mc == 0)
		return; /* no matches — VGA has no bell, so just do nothing */

	/* Compute the longest common prefix of all matches */
	int cp = common_prefix_len(matches, mc);

	/* How many chars to write: the full match for a single result, the
     * common prefix for multiple results */
	int write_len = (mc == 1) ? (int)strlen(matches[0]) : cp;

	if (write_len > name_pfx_len) {
		/* Extend the in-memory word and echo only the newly completed tail.
         * This avoids relying on backspace-driven redraw accuracy. */
		if (word_start + name_start + write_len < max - 1) {
			for (int i = 0; i < write_len; i++)
				word[name_start + i] = matches[0][i];
			*n = word_start + name_start + write_len;
			buf[*n] = '\0';
		}

		/* Echo just the suffix past what the user already typed. */
		for (int i = name_pfx_len; i < write_len; i++)
			putchar(matches[0][i]);
	}

	/* If there are multiple matches, display them and redraw the input line */
	if (mc > 1) {
		putchar('\n');
		for (int i = 0; i < mc; i++)
			printf("%s  ", matches[i]);
		putchar('\n');
		print_prompt();
		for (int i = 0; i < *n; i++)
			putchar(buf[i]);
	}
}

/* ── job control ───────────────────────────────────────────────────────── */

#define MAX_JOBS 8

typedef struct {
	int pgid;     /* process group ID (0 = slot free) */
	int pid1;     /* first child we wait on */
	int pid2;     /* optional second child (pipeline) */
	int stopped;  /* 1 = stopped, 0 = running in background */
	char cmd[64]; /* command name for display */
} job_t;

static job_t jobs[MAX_JOBS];

static void job_copy_cmd(char *dst, const char *src)
{
	strncpy(dst, src, 63);
	dst[63] = '\0';
}

static void job_clear(job_t *j)
{
	j->pgid = 0;
	j->pid1 = 0;
	j->pid2 = 0;
	j->stopped = 0;
	j->cmd[0] = '\0';
}

static void job_add(int pgid, int pid1, int pid2, const char *cmd, int stopped)
{
	for (int i = 0; i < MAX_JOBS; i++) {
		if (jobs[i].pgid == 0) {
			jobs[i].pgid = pgid;
			jobs[i].pid1 = pid1;
			jobs[i].pid2 = pid2;
			jobs[i].stopped = stopped;
			job_copy_cmd(jobs[i].cmd, cmd);
			return;
		}
	}
	printf("jobs: table full\n");
}

static void job_remove(job_t *j)
{
	job_clear(j);
}

static job_t *job_find_by_num(int num)
{
	/* Job numbers are 1-based indices into the jobs array */
	if (num < 1 || num > MAX_JOBS)
		return 0;
	if (jobs[num - 1].pgid == 0)
		return 0;
	return &jobs[num - 1];
}

static int job_num_of(job_t *j)
{
	return (int)(j - jobs) + 1;
}

/* Find the latest job (highest occupied index). */
static job_t *job_find_latest(void)
{
	for (int i = MAX_JOBS - 1; i >= 0; i--) {
		if (jobs[i].pgid != 0)
			return &jobs[i];
	}
	return 0;
}

static void
pipeline_cmd(char *buf, int bufsz, const char *left, const char *right)
{
	snprintf(buf, (size_t)bufsz, "%s | %s", left, right);
}

static void job_note_status(job_t *j, int which, int status, int allow_zero)
{
	int *pid = (which == 1) ? &j->pid1 : &j->pid2;

	if (status == 0 && !allow_zero)
		return;
	if (status < 0) {
		*pid = 0;
		return;
	}
	if (WIFSTOPPED(status)) {
		j->stopped = 1;
		return;
	}

	print_signal_status(status);
	*pid = 0;
}

static int job_wait_foreground(job_t *j)
{
	j->stopped = 0;

	if (j->pid1 > 0)
		job_note_status(j, 1, sys_waitpid(j->pid1, WUNTRACED), 1);
	if (j->pid2 > 0)
		job_note_status(j, 2, sys_waitpid(j->pid2, WUNTRACED), 1);

	return j->stopped;
}

static void job_refresh(job_t *j)
{
	if (j->pid1 > 0)
		job_note_status(j, 1, sys_waitpid(j->pid1, WNOHANG | WUNTRACED), 0);
	if (j->pid2 > 0)
		job_note_status(j, 2, sys_waitpid(j->pid2, WNOHANG | WUNTRACED), 0);

	if (j->pid1 == 0 && j->pid2 == 0)
		job_remove(j);
}

static void cmd_jobs(void)
{
	int any = 0;
	for (int i = 0; i < MAX_JOBS; i++) {
		if (jobs[i].pgid == 0)
			continue;

		/* Refresh exit/stop state before displaying the table entry. */
		job_refresh(&jobs[i]);
		if (jobs[i].pgid == 0)
			continue;

		any = 1;
		printf("[%d] %s  %s\n",
		       i + 1,
		       jobs[i].stopped ? "Stopped" : "Running",
		       jobs[i].cmd);
	}
	if (!any)
		printf("No jobs.\n");
}

static void cmd_fg(int argc, char **argv)
{
	job_t *j;
	if (argc > 1) {
		/* atoi tolerates non-digit input by stopping early; be strict here
         * so that "fg abc" reports "no such job" rather than picking job 0. */
		int num = 0, ok = 1;
		for (const char *p = argv[1]; *p; p++) {
			if (*p < '0' || *p > '9') {
				ok = 0;
				break;
			}
			num = num * 10 + (*p - '0');
		}
		j = ok ? job_find_by_num(num) : 0;
	} else {
		j = job_find_latest();
	}
	if (!j) {
		printf("fg: no such job\n");
		return;
	}

	job_refresh(j);
	if (j->pgid == 0) {
		printf("fg: no such job\n");
		return;
	}

	printf("%s\n", j->cmd);

	/* Give the child's group the terminal */
	sys_tcsetpgrp(0, j->pgid);

	/* Resume the process if it was stopped */
	if (j->stopped) {
		j->stopped = 0;
		sys_kill(-j->pgid, SIGCONT);
	}

	/* Wait for the foreground job to either stop again or fully exit. */
	int stopped = job_wait_foreground(j);

	shell_claim_foreground_tty();

	if (stopped)
		printf("[%d] Stopped  %s\n", job_num_of(j), j->cmd);
	else
		job_remove(j);
}

static void cmd_bg(int argc, char **argv)
{
	job_t *j;
	if (argc > 1) {
		int num = 0, ok = 1;
		for (const char *p = argv[1]; *p; p++) {
			if (*p < '0' || *p > '9') {
				ok = 0;
				break;
			}
			num = num * 10 + (*p - '0');
		}
		j = ok ? job_find_by_num(num) : 0;
	} else {
		j = job_find_latest();
	}
	if (!j || !j->stopped) {
		if (j) {
			job_refresh(j);
			if (j->pgid != 0 && j->stopped)
				goto resume_bg;
		}
		printf("bg: no stopped job\n");
		return;
	}

resume_bg:
	j->stopped = 0;
	sys_kill(-j->pgid, SIGCONT);
	printf("[%d] %s &\n", job_num_of(j), j->cmd);
}

/* ── entry point ────────────────────────────────────────────────────────── */

static void run_program(char **argv, int argc)
{
	char cmd_name[64];
	char exec_path[128];

	job_copy_cmd(cmd_name, argv[0]);

	if (resolve_command_path(argv[0], exec_path, sizeof(exec_path)) != 0) {
		printf("not found: %s\n", cmd_name);
		return;
	}

	int pid = sys_fork();
	if (pid < 0) {
		printf("fork failed\n");
		return;
	}
	if (pid == 0) {
		char *exec_envp[MAX_ENV_VARS + 1];
		env_compose_exec(exec_envp, MAX_ENV_VARS + 1);
		shell_reset_job_signal_state();
		sys_setpgid(0, 0);
		(void)argc;
		sys_execve(exec_path, argv, exec_envp);
		printf("exec failed: %s\n", exec_path);
		sys_exit(127);
	}

	/* Put the child in its own process group and give it the terminal */
	sys_setpgid(pid, pid);
	sys_tcsetpgrp(0, pid);

	job_t fg_job;
	fg_job.pgid = pid;
	fg_job.pid1 = pid;
	fg_job.pid2 = 0;
	fg_job.stopped = 0;
	job_copy_cmd(fg_job.cmd, cmd_name);

	/* Wait for the child, also returning if it stops */
	int stopped = job_wait_foreground(&fg_job);
	shell_claim_foreground_tty();

	if (stopped) {
		/* Child was stopped (e.g. by Ctrl+Z) — add to the job list. */
		job_add(pid, pid, 0, cmd_name, 1);
		int jobnum = 0;
		for (int i = 0; i < MAX_JOBS; i++) {
			if (jobs[i].pgid == pid) {
				jobnum = i + 1;
				break;
			}
		}
		if (jobnum > 0)
			printf("[%d] Stopped  %s\n", jobnum, cmd_name);
		else
			printf("Stopped  %s\n", cmd_name);
	}
}

static int exec_replace_self(char **argv, int argc)
{
	char exec_path[128];
	char *exec_envp[MAX_ENV_VARS + 1];
	int exec_envc;

	if (argc < 2) {
		printf("usage: exec <command> [args...]\n");
		return 2;
	}

	if (resolve_command_path(argv[1], exec_path, sizeof(exec_path)) != 0) {
		printf("not found: %s\n", argv[1]);
		return 127;
	}

	exec_envc = env_compose_exec(exec_envp, MAX_ENV_VARS + 1);
	(void)exec_envc;
	shell_reset_job_signal_state();
	sys_execve(exec_path, &argv[1], exec_envp);
	printf("exec failed: %s\n", exec_path);
	shell_install_signal_state();
	return 126;
}

/*
 * run_piped: run two external programs connected by a pipe.
 *
 * Forks two children. The left child wires its stdout to the write end of the
 * pipe and execs lv[0]. The right child wires its stdin to the read end and
 * execs rv[0]. The parent closes both pipe ends and waits for both children.
 */
static void run_piped(char **lv,
                      int lc,
                      const shell_redir_t *lr,
                      char **rv,
                      int rc,
                      const shell_redir_t *rr)
{
	char cmd[64];
	int fds[2];
	if (sys_pipe(fds) != 0) {
		printf("pipe: failed\n");
		return;
	}

	/* Fork the left (writer) child. */
	int lpid = sys_fork();
	if (lpid < 0) {
		printf("pipe: fork failed\n");
		sys_close(fds[0]);
		sys_close(fds[1]);
		return;
	}
	if (lpid == 0) {
		char exec_path[128];
		char *exec_envp[MAX_ENV_VARS + 1];
		env_compose_exec(exec_envp, MAX_ENV_VARS + 1);
		shell_reset_job_signal_state();
		sys_setpgid(0, 0);
		/* Child: redirect stdout to the write end, then exec the left command.
         * After exec the child's image is replaced; the exec'd process inherits
         * fd 1 = pipe write end.  Close the read end so it doesn't leak. */
		sys_dup2(fds[1], 1);
		sys_close(fds[0]);
		sys_close(fds[1]);
		if (shell_redir_apply(lr) != 0)
			sys_exit(1);
		if (resolve_command_path(lv[0], exec_path, sizeof(exec_path)) != 0) {
			printf("pipe: left exec failed\n");
			sys_exit(1);
		}
		(void)lc;
		sys_execve(exec_path, lv, exec_envp);
		printf("pipe: left exec failed: %s\n", exec_path);
		sys_exit(1);
	}

	/* Fork the right (reader) child. */
	int rpid = sys_fork();
	if (rpid < 0) {
		printf("pipe: fork failed\n");
		sys_close(fds[0]);
		sys_close(fds[1]);
		sys_wait(lpid);
		return;
	}
	if (rpid == 0) {
		char exec_path[128];
		char *exec_envp[MAX_ENV_VARS + 1];
		env_compose_exec(exec_envp, MAX_ENV_VARS + 1);
		shell_reset_job_signal_state();
		sys_setpgid(0, lpid);
		/* Child: redirect stdin to the read end, then exec the right command. */
		sys_dup2(fds[0], 0);
		sys_close(fds[0]);
		sys_close(fds[1]);
		if (shell_redir_apply(rr) != 0)
			sys_exit(1);
		if (resolve_command_path(rv[0], exec_path, sizeof(exec_path)) != 0) {
			printf("pipe: right exec failed\n");
			sys_exit(1);
		}
		(void)rc;
		sys_execve(exec_path, rv, exec_envp);
		printf("pipe: right exec failed: %s\n", exec_path);
		sys_exit(1);
	}

	/* Parent: close both pipe ends (so the write end reaches zero refs when
     * the left child exits, delivering EOF to the right child), then wait. */
	sys_close(fds[0]);
	sys_close(fds[1]);
	sys_setpgid(lpid, lpid);
	sys_setpgid(rpid, lpid);
	sys_tcsetpgrp(0, lpid);

	job_t fg_job;
	fg_job.pgid = lpid;
	fg_job.pid1 = lpid;
	fg_job.pid2 = rpid;
	fg_job.stopped = 0;
	pipeline_cmd(cmd, sizeof(cmd), lv[0], rv[0]);
	job_copy_cmd(fg_job.cmd, cmd);

	int stopped = job_wait_foreground(&fg_job);
	shell_claim_foreground_tty();

	if (stopped) {
		job_add(lpid, fg_job.pid1, fg_job.pid2, cmd, 1);
		int jobnum = 0;
		for (int i = 0; i < MAX_JOBS; i++) {
			if (jobs[i].pgid == lpid) {
				jobnum = i + 1;
				break;
			}
		}
		if (jobnum > 0) {
			printf("[%d] Stopped  %s\n", jobnum, cmd);
		} else {
			printf("Stopped  %s\n", cmd);
		}
	}
}

static int parse_number(const char *s, int *out)
{
	int sign = 1;
	int n = 0;

	if (!s || !*s)
		return 0;
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+')
		s++;
	if (!*s)
		return 0;
	while (*s) {
		if (*s < '0' || *s > '9')
			return 0;
		n = n * 10 + (*s - '0');
		s++;
	}
	*out = sign * n;
	return 1;
}

static int signal_number(const char *name)
{
	if (!strcmp(name, "HUP"))
		return 1;
	if (!strcmp(name, "INT"))
		return SIGINT;
	if (!strcmp(name, "ILL"))
		return SIGILL;
	if (!strcmp(name, "TRAP"))
		return SIGTRAP;
	if (!strcmp(name, "ABRT"))
		return SIGABRT;
	if (!strcmp(name, "FPE"))
		return SIGFPE;
	if (!strcmp(name, "KILL"))
		return SIGKILL;
	if (!strcmp(name, "SEGV"))
		return SIGSEGV;
	if (!strcmp(name, "PIPE"))
		return SIGPIPE;
	if (!strcmp(name, "TERM"))
		return SIGTERM;
	if (!strcmp(name, "CHLD"))
		return SIGCHLD;
	if (!strcmp(name, "CONT"))
		return SIGCONT;
	if (!strcmp(name, "STOP"))
		return SIGSTOP;
	if (!strcmp(name, "TSTP"))
		return SIGTSTP;
	if (!strncmp(name, "SIG", 3))
		return signal_number(name + 3);

	int n;
	if (parse_number(name, &n))
		return n;
	return -1;
}

static int cmd_kill(int argc, char **argv)
{
	int sig = SIGTERM;
	int first_pid = 1;

	if (argc < 2) {
		printf("usage: kill [-s signal|-signal] pid...\n");
		return 2;
	}

	if (!strcmp(argv[1], "-s")) {
		if (argc < 4) {
			printf("usage: kill -s signal pid...\n");
			return 2;
		}
		sig = signal_number(argv[2]);
		first_pid = 3;
	} else if (argv[1][0] == '-' && argv[1][1] != '\0') {
		sig = signal_number(argv[1] + 1);
		first_pid = 2;
	}

	if (sig <= 0) {
		printf("kill: invalid signal\n");
		return 2;
	}

	int rc = 0;
	for (int i = first_pid; i < argc; i++) {
		int pid;
		if (!parse_number(argv[i], &pid) || pid == 0) {
			printf("kill: invalid pid: %s\n", argv[i]);
			rc = 1;
			continue;
		}
		if (sys_kill(pid, sig) != 0) {
			printf("kill: failed: %s\n", argv[i]);
			rc = 1;
		}
	}
	return rc;
}

static int wait_one_job(job_t *j)
{
	if (!j || j->pgid == 0)
		return 127;

	sys_tcsetpgrp(0, j->pgid);
	int stopped = job_wait_foreground(j);
	shell_claim_foreground_tty();

	if (stopped) {
		printf("[%d] Stopped  %s\n", job_num_of(j), j->cmd);
		return 128 + SIGTSTP;
	}
	job_remove(j);
	return 0;
}

static int cmd_wait(int argc, char **argv)
{
	int rc = 0;

	if (argc == 1) {
		for (int i = 0; i < MAX_JOBS; i++)
			if (jobs[i].pgid != 0)
				rc = wait_one_job(&jobs[i]);
		return rc;
	}

	for (int i = 1; i < argc; i++) {
		int num;
		job_t *j;
		if (argv[i][0] == '%') {
			if (!parse_number(argv[i] + 1, &num))
				j = 0;
			else
				j = job_find_by_num(num);
		} else if (parse_number(argv[i], &num))
			j = job_find_by_num(num);
		else
			j = 0;

		if (!j) {
			printf("wait: no such job: %s\n", argv[i]);
			rc = 127;
		} else
			rc = wait_one_job(j);
	}
	return rc;
}

static int is_shell_builtin(const char *name)
{
	static const char *builtins[] = {
	    ".",       ":",        "[",    "alias",    "bg",     "break",  "cd",
	    "command", "continue", "echo", "eval",     "exec",   "exit",   "export",
	    "false",   "fc",       "fg",   "getopts",  "jobs",   "kill",   "newgrp",
	    "printf",  "pwd",      "read", "readonly", "return", "set",    "shift",
	    "test",    "times",    "trap", "true",     "type",   "ulimit", "umask",
	    "unalias", "unset",    "wait", 0};

	for (int i = 0; builtins[i]; i++)
		if (!strcmp(name, builtins[i]))
			return 1;
	return 0;
}

static int cmd_type(int argc, char **argv)
{
	int rc = 0;

	if (argc < 2) {
		printf("usage: type name...\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		char resolved[128];
		if (is_shell_builtin(argv[i]))
			printf("%s is a shell builtin\n", argv[i]);
		else if (resolve_command_path(argv[i], resolved, sizeof(resolved)) == 0)
			printf("%s is %s\n", argv[i], resolved);
		else {
			printf("%s: not found\n", argv[i]);
			rc = 1;
		}
	}
	return rc;
}

static int cmd_command(int argc, char **argv)
{
	if (argc < 2)
		return 0;

	if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "-V")) {
		int verbose = !strcmp(argv[1], "-V");
		int rc = 0;
		for (int i = 2; i < argc; i++) {
			char resolved[128];
			if (is_shell_builtin(argv[i]))
				printf(verbose ? "%s is a shell builtin\n" : "%s\n", argv[i]);
			else if (resolve_command_path(
			             argv[i], resolved, sizeof(resolved)) == 0)
				printf(verbose ? "%s is %s\n" : "%s\n", argv[i], resolved);
			else
				rc = 1;
		}
		return rc;
	}

	printf(
	    "command: executing utilities through command is not supported yet\n");
	printf("command: this needs shared builtin dispatch and PATH lookup "
	       "semantics\n");
	return 2;
}

static int streq(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

static int
test_binary_int(const char *a, const char *op, const char *b, int *ok)
{
	int ai, bi;
	*ok = parse_number(a, &ai) && parse_number(b, &bi);
	if (!*ok)
		return 0;
	if (streq(op, "-eq"))
		return ai == bi;
	if (streq(op, "-ne"))
		return ai != bi;
	if (streq(op, "-gt"))
		return ai > bi;
	if (streq(op, "-ge"))
		return ai >= bi;
	if (streq(op, "-lt"))
		return ai < bi;
	if (streq(op, "-le"))
		return ai <= bi;
	*ok = 0;
	return 0;
}

static int cmd_test_eval(int argc, char **argv)
{
	if (argc == 0)
		return 1;
	if (argc == 1)
		return argv[0][0] ? 0 : 1;
	if (argc == 2) {
		if (streq(argv[0], "!"))
			return cmd_test_eval(1, &argv[1]) == 0 ? 1 : 0;
		if (streq(argv[0], "-n"))
			return argv[1][0] ? 0 : 1;
		if (streq(argv[0], "-z"))
			return argv[1][0] ? 1 : 0;
		printf("test: unsupported unary operator: %s\n", argv[0]);
		return 2;
	}
	if (argc == 3) {
		if (streq(argv[1], "="))
			return streq(argv[0], argv[2]) ? 0 : 1;
		if (streq(argv[1], "!="))
			return !streq(argv[0], argv[2]) ? 0 : 1;
		if (argv[1][0] == '-' && argv[1][1]) {
			int ok;
			int r = test_binary_int(argv[0], argv[1], argv[2], &ok);
			if (ok)
				return r ? 0 : 1;
		}
		printf("test: unsupported binary operator: %s\n", argv[1]);
		return 2;
	}

	printf("test: expressions with more than 3 terms are not supported yet\n");
	return 2;
}

static int cmd_test(int argc, char **argv)
{
	return cmd_test_eval(argc - 1, &argv[1]);
}

static int cmd_bracket(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
		printf("[: missing ]\n");
		return 2;
	}
	return cmd_test_eval(argc - 2, &argv[1]);
}

static void cmd_not_supported(const char *name, const char *reason)
{
	printf("%s: not supported in this shell yet\n", name);
	printf("%s: %s\n", name, reason);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	env_import(environ);

	/* Set up job control: make the shell its own process group, claim the
     * controlling terminal for this session, and handle prompt-time signals
     * explicitly instead of relying on a kernel-side fallback path. */
	sys_setpgid(0, 0); /* pgid = own pid */
	shell_install_signal_state();
	shell_claim_foreground_tty();

	/* Ensure TTY is in Raw mode so shell handles its own echo/backspace */
	termios_t t;
	if (sys_tcgetattr(0, &t) == 0) {
		t.c_lflag &= ~(ICANON | ECHO);
		sys_tcsetattr(0, 0, &t);
	}

	printf(TERM_COLOR_YELLOW
	       "drunix shell -- type 'help' for commands\n" TERM_COLOR_RESET);

	char line[128];
	char *tokens[SHELL_MAX_ARGS];
	for (;;) {
		/* Prompt: "drunix:/> " at root, "drunix:/tests> " in a subdir. */
		g_prompt_signal = 0;
		shell_install_signal_state();
		shell_claim_foreground_tty();
		print_prompt();

		int n = readline(line, sizeof(line));
		if (n == 0)
			continue;
		shell_history_add(line);

		/* Tokenize the line in place so each token is a C string and
         * `tokens[]` holds argv pointers into the `line` buffer. */
		int tc = tokenize(line, tokens, SHELL_MAX_ARGS);
		if (tc == 0)
			continue;

		/* Scan for a pipe operator.  If found, split the token array in place
         * and run both sides connected by a kernel pipe.  Only external ELF
         * programs can be piped — built-ins are not supported on either side. */
		int pipe_pos = -1;
		for (int i = 0; i < tc; i++) {
			if (tokens[i][0] == '|' && tokens[i][1] == '\0') {
				pipe_pos = i;
				break;
			}
		}
		if (pipe_pos > 0 && pipe_pos < tc - 1) {
			shell_redir_t left_redir;
			shell_redir_t right_redir;
			int lc = pipe_pos;
			int rc = tc - pipe_pos - 1;

			tokens[pipe_pos] = (char *)0; /* NUL-terminate the left argv */
			if (shell_parse_redirections(tokens, &lc, &left_redir) != 0 ||
			    shell_parse_redirections(
			        &tokens[pipe_pos + 1], &rc, &right_redir) != 0) {
				last_status = 2;
				continue;
			}
			run_piped(tokens,
			          lc,
			          &left_redir,
			          &tokens[pipe_pos + 1],
			          rc,
			          &right_redir);
			continue;
		}

		shell_redir_t redir;
		shell_redir_state_t redir_state;
		if (shell_parse_redirections(tokens, &tc, &redir) != 0) {
			last_status = 2;
			continue;
		}
		if (shell_redir_begin(&redir, &redir_state) != 0) {
			last_status = 1;
			continue;
		}

		/* Built-ins dispatch on the first token. */
		if (!strcmp(tokens[0], "help")) {
			cmd_help();
			last_status = 0;
		} else if (!strcmp(tokens[0], ":")) {
			last_status = 0;
		} else if (!strcmp(tokens[0], "true")) {
			last_status = 0;
		} else if (!strcmp(tokens[0], "false")) {
			last_status = 1;
		} else if (!strcmp(tokens[0], "exit")) {
			int status = 0;
			if (tc > 2) {
				printf("exit: too many arguments\n");
				last_status = 2;
			} else if (tc == 2 && !parse_exit_status(tokens[1], &status)) {
				printf("exit: numeric argument required: %s\n", tokens[1]);
				status = 2;
				sys_exit(status);
			} else {
				sys_exit(status);
			}
		} else if (!strcmp(tokens[0], "readonly")) {
			cmd_readonly(tc, tokens);
			last_status = 0;
		} else if (!strcmp(tokens[0], "read")) {
			cmd_read(tc, tokens);
			last_status = 0;
		} else if (!strcmp(tokens[0], "kill")) {
			last_status = cmd_kill(tc, tokens);
		} else if (!strcmp(tokens[0], "wait")) {
			last_status = cmd_wait(tc, tokens);
		} else if (!strcmp(tokens[0], "type")) {
			last_status = cmd_type(tc, tokens);
		} else if (!strcmp(tokens[0], "command")) {
			last_status = cmd_command(tc, tokens);
		} else if (!strcmp(tokens[0], "test")) {
			last_status = cmd_test(tc, tokens);
		} else if (!strcmp(tokens[0], "[")) {
			last_status = cmd_bracket(tc, tokens);
		} else if (!strcmp(tokens[0], "pwd")) {
			cmd_pwd();
			last_status = 0;
		} else if (!strcmp(tokens[0], "echo")) {
			cmd_echo(tc, tokens);
			last_status = 0;
		} else if (!strcmp(tokens[0], "export")) {
			cmd_export(tc, tokens);
			last_status = 0;
		} else if (!strcmp(tokens[0], "unset")) {
			cmd_unset(tc, tokens);
			last_status = 0;
		} else if (!strcmp(tokens[0], "ls")) {
			cmd_ls(tc > 1 ? tokens[1] : 0);
			last_status = 0;
		} else if (!strcmp(tokens[0], "cd")) {
			cmd_cd(tc > 1 ? tokens[1] : 0);
			last_status = 0;
		} else if (!strcmp(tokens[0], "mkdir")) {
			if (tc < 2)
				printf("usage: mkdir <dirname>\n");
			else
				cmd_mkdir(tokens[1]);
			last_status = 0;
		} else if (!strcmp(tokens[0], "rmdir")) {
			if (tc < 2)
				printf("usage: rmdir <dirname>\n");
			else
				cmd_rmdir(tokens[1]);
			last_status = 0;
		} else if (!strcmp(tokens[0], "clear")) {
			sys_clear();
			last_status = 0;
		} else if (!strcmp(tokens[0], "cat")) {
			if (tc < 2)
				cmd_cat_fd(0);
			else
				cmd_cat(tokens[1]);
			last_status = 0;
		} else if (!strcmp(tokens[0], "rm")) {
			if (tc < 2)
				printf("usage: rm <filename>\n");
			else
				cmd_rm(tokens[1]);
			last_status = 0;
		} else if (!strcmp(tokens[0], "cp")) {
			if (tc < 3)
				printf("usage: cp <src> <dst>\n");
			else
				cmd_cp(tokens[1], tokens[2]);
			last_status = 0;
		} else if (!strcmp(tokens[0], "mv")) {
			if (tc < 3)
				printf("usage: mv <src> <dst>\n");
			else
				cmd_mv(tokens[1], tokens[2]);
			last_status = 0;
		} else if (!strcmp(tokens[0], "exec")) {
			last_status = exec_replace_self(tokens, tc);
		} else if (!strcmp(tokens[0], "modload")) {
			if (tc < 2) {
				printf("usage: modload <filename>\n");
			} else {
				int rc = sys_modload(tokens[1]);
				if (rc != 0)
					printf("modload: failed (%d)\n", -rc);
			}
			last_status = 0;
		} else if (!strcmp(tokens[0], "jobs")) {
			cmd_jobs();
			last_status = 0;
		} else if (!strcmp(tokens[0], "fg")) {
			cmd_fg(tc, tokens);
			last_status = 0;
		} else if (!strcmp(tokens[0], "bg")) {
			cmd_bg(tc, tokens);
			last_status = 0;
		} else if (!strcmp(tokens[0], ".")) {
			cmd_not_supported(".",
			                  "sourcing requires a script parser/executor, not "
			                  "just interactive token dispatch");
			last_status = 127;
		} else if (!strcmp(tokens[0], "break") ||
		           !strcmp(tokens[0], "continue") ||
		           !strcmp(tokens[0], "return") ||
		           !strcmp(tokens[0], "shift")) {
			cmd_not_supported(tokens[0],
			                  "requires shell script/function execution state");
			last_status = 127;
		} else if (!strcmp(tokens[0], "eval")) {
			cmd_not_supported("eval",
			                  "requires a real parser with quoting, expansion, "
			                  "and recursive command execution");
			last_status = 127;
		} else if (!strcmp(tokens[0], "alias") ||
		           !strcmp(tokens[0], "unalias")) {
			cmd_not_supported(tokens[0],
			                  "requires parser-level alias expansion; "
			                  "single-token replacement would be a shortcut");
			last_status = 127;
		} else if (!strcmp(tokens[0], "fc")) {
			cmd_not_supported("fc",
			                  "requires persistent editable command history");
			last_status = 127;
		} else if (!strcmp(tokens[0], "getopts")) {
			cmd_not_supported("getopts",
			                  "requires shell positional parameters and script "
			                  "execution state");
			last_status = 127;
		} else if (!strcmp(tokens[0], "newgrp")) {
			cmd_not_supported("newgrp",
			                  "requires users, groups, credentials, and "
			                  "permission checks in the kernel");
			last_status = 127;
		} else if (!strcmp(tokens[0], "printf")) {
			cmd_not_supported(
			    "printf",
			    "POSIX printf needs full format and escape handling; adding a "
			    "tiny subset would be a shortcut");
			last_status = 127;
		} else if (!strcmp(tokens[0], "set")) {
			cmd_not_supported(
			    "set", "requires shell options and positional parameters");
			last_status = 127;
		} else if (!strcmp(tokens[0], "times")) {
			cmd_not_supported("times",
			                  "requires per-process user/system CPU accounting "
			                  "in the kernel");
			last_status = 127;
		} else if (!strcmp(tokens[0], "trap")) {
			cmd_not_supported("trap",
			                  "requires parser-backed action strings and "
			                  "shell-level signal dispatch");
			last_status = 127;
		} else if (!strcmp(tokens[0], "ulimit")) {
			cmd_not_supported("ulimit", "requires kernel resource limits");
			last_status = 127;
		} else if (!strcmp(tokens[0], "umask")) {
			cmd_not_supported(
			    "umask",
			    "requires filesystem permission bits and process umask state");
			last_status = 127;
		} else {
			/* No recognized built-in — try to run it as a program. */
			run_program(tokens, tc);
			last_status = 0;
		}

		shell_redir_end(&redir_state);
	}

	return 0;
}

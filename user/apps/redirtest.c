/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * redirtest.c - shell redirection smoke test.
 */

#include "string.h"
#include "syscall.h"

static int write_all(int fd, const char *buf, int len)
{
	int off = 0;

	while (off < len) {
		int n = sys_fwrite(fd, buf + off, len - off);
		if (n <= 0)
			return -1;
		off += n;
	}
	return 0;
}

static int read_file(const char *path, char *buf, int cap)
{
	int fd = sys_open(path);
	int total = 0;

	if (fd < 0)
		return -1;
	while (total < cap - 1) {
		int n = sys_read(fd, buf + total, cap - 1 - total);
		if (n < 0) {
			sys_close(fd);
			return -1;
		}
		if (n == 0)
			break;
		total += n;
	}
	buf[total] = '\0';
	sys_close(fd);
	return total;
}

static void log_line(int fd, const char *msg)
{
	write_all(fd, msg, (int)strlen(msg));
	write_all(fd, "\n", 1);
}

int main(void)
{
	static const char script[] =
	    "echo first > /dufs/redir.txt\n"
	    "echo second >> /dufs/redir.txt\n"
	    "wc -l < /dufs/redir.txt > /dufs/redir.wc\n"
	    "cat < /dufs/redir.txt > /dufs/redir.copy\n"
	    "cat < /dufs/redir.txt | wc -l > /dufs/redir.pipewc\n"
	    "exit\n";
	char *argv[] = {"shell", 0};
	char *envp[] = {"PATH=/bin", 0};
	char buf[128];
	int pipefd[2];
	int pid;
	int status;
	int logfd;
	int ok = 1;

	sys_unlink("/dufs/redir.txt");
	sys_unlink("/dufs/redir.wc");
	sys_unlink("/dufs/redir.copy");
	sys_unlink("/dufs/redir.pipewc");
	sys_unlink("/dufs/redirtest.log");

	logfd = sys_create("/dufs/redirtest.log");
	if (logfd < 0)
		return 1;

	if (sys_pipe(pipefd) != 0) {
		log_line(logfd, "FAIL pipe");
		sys_close(logfd);
		return 1;
	}

	pid = sys_fork();
	if (pid < 0) {
		log_line(logfd, "FAIL fork");
		sys_close(pipefd[0]);
		sys_close(pipefd[1]);
		sys_close(logfd);
		return 1;
	}

	if (pid == 0) {
		sys_dup2(pipefd[0], 0);
		sys_close(pipefd[0]);
		sys_close(pipefd[1]);
		sys_execve("/bin/shell", argv, envp);
		sys_exit(127);
	}

	sys_close(pipefd[0]);
	if (write_all(pipefd[1], script, (int)strlen(script)) != 0)
		ok = 0;
	sys_close(pipefd[1]);

	status = sys_waitpid(pid, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		log_line(logfd, "FAIL shell status");
		ok = 0;
	}

	if (read_file("/dufs/redir.txt", buf, sizeof(buf)) < 0 ||
	    strcmp(buf, "first\nsecond\n") != 0) {
		log_line(logfd, "FAIL output redirection");
		ok = 0;
	}

	if (read_file("/dufs/redir.wc", buf, sizeof(buf)) < 0 ||
	    strcmp(buf, "2\n") != 0) {
		log_line(logfd, "FAIL input redirection");
		ok = 0;
	}

	if (read_file("/dufs/redir.copy", buf, sizeof(buf)) < 0 ||
	    strcmp(buf, "first\nsecond\n") != 0) {
		log_line(logfd, "FAIL builtin input redirection");
		ok = 0;
	}

	if (read_file("/dufs/redir.pipewc", buf, sizeof(buf)) < 0 ||
	    strcmp(buf, "2\n") != 0) {
		log_line(logfd, "FAIL pipeline redirection");
		ok = 0;
	}

	log_line(logfd, ok ? "REDIRTEST PASS" : "REDIRTEST FAIL");
	sys_close(logfd);
	return ok ? 0 : 1;
}

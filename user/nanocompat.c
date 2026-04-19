/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * nanocompat.c - unattended GNU nano/Linux i386 compatibility runner.
 */

#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/syscall.h"

#define OUT_CAP 8192

static int log_fd = -1;

static int text_contains(const char *haystack, const char *needle)
{
    int hlen;
    int nlen;

    if (!needle || needle[0] == '\0')
        return 1;
    if (!haystack)
        return 0;

    hlen = (int)strlen(haystack);
    nlen = (int)strlen(needle);
    if (nlen > hlen)
        return 0;

    for (int i = 0; i <= hlen - nlen; i++) {
        if (strncmp(haystack + i, needle, (unsigned int)nlen) == 0)
            return 1;
    }
    return 0;
}

static void emit(const char *s)
{
    int len = (int)strlen(s);

    sys_fwrite(1, s, len);
    if (log_fd >= 0)
        sys_fwrite(log_fd, s, len);
}

static void emitf(const char *fmt, const char *tag, int a, int b)
{
    char buf[256];

    snprintf(buf, sizeof(buf), fmt, tag, a, b);
    emit(buf);
}

static int wait_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status))
        return 128 + WSTOPSIG(status);
    return 255;
}

static int run_capture_input(const char *path, char **argv, char **envp,
                             const char *input, char *out, int out_cap)
{
    int inpipe[2];
    int outpipe[2];
    int pid;
    int used = 0;

    if (sys_pipe(inpipe) != 0)
        return 255;
    if (sys_pipe(outpipe) != 0) {
        sys_close(inpipe[0]);
        sys_close(inpipe[1]);
        return 255;
    }

    pid = sys_fork();
    if (pid == 0) {
        sys_dup2(inpipe[0], 0);
        sys_dup2(outpipe[1], 1);
        sys_dup2(outpipe[1], 2);
        sys_close(inpipe[0]);
        sys_close(inpipe[1]);
        sys_close(outpipe[0]);
        sys_close(outpipe[1]);
        sys_execve(path, argv, envp);
        sys_write("exec failed\n");
        sys_exit(127);
    }

    if (pid < 0) {
        sys_close(inpipe[0]);
        sys_close(inpipe[1]);
        sys_close(outpipe[0]);
        sys_close(outpipe[1]);
        return 255;
    }

    sys_close(inpipe[0]);
    sys_close(outpipe[1]);
    if (input)
        sys_fwrite(inpipe[1], input, (int)strlen(input));
    sys_close(inpipe[1]);

    for (;;) {
        char chunk[256];
        int n = sys_read(outpipe[0], chunk, sizeof(chunk));

        if (n <= 0)
            break;
        if (used < out_cap - 1) {
            int room = out_cap - 1 - used;
            int copy = n < room ? n : room;

            memcpy(out + used, chunk, (unsigned int)copy);
            used += copy;
        }
    }
    out[used] = '\0';
    sys_close(outpipe[0]);

    return wait_exit_code(sys_waitpid(pid, 0));
}

static int stage_ok(const char *stage, const char *path, char **argv, char **envp,
                    const char *input, const char *must_contain,
                    char *out, int out_cap)
{
    int code = run_capture_input(path, argv, envp, input, out, out_cap);
    int ok = code == 0 && text_contains(out, must_contain);

    if (ok) {
        emit(stage);
        emit(" ok\n");
        return 0;
    }

    emitf("NANOCOMPAT FAIL %s exit=%d expected=0\n", stage, code, 0);
    emit("NANOCOMPAT OUTPUT BEGIN\n");
    emit(out);
    emit("NANOCOMPAT OUTPUT END\n");
    return -1;
}

static int read_text_file(const char *path, char *buf, int cap)
{
    int fd;
    int n;

    fd = sys_open(path);
    if (fd < 0)
        return -1;
    n = sys_read(fd, buf, cap - 1);
    if (n < 0) {
        sys_close(fd);
        return -1;
    }
    buf[n] = '\0';
    sys_close(fd);
    return n;
}

static int nano_write_ok(char **argv, char **envp, const char *input,
                         char *out, int out_cap)
{
    int code = run_capture_input("/bin/nano", argv, envp, input, out, out_cap);

    if (read_text_file("/tmp/nano-write.txt", out, out_cap) >= 0 &&
        text_contains(out, "NANOCOMPAT FILE OK"))
        return 0;
    if (read_text_file("/tmp/nano-write.txt.save", out, out_cap) >= 0 &&
        text_contains(out, "NANOCOMPAT FILE OK")) {
        char msg[96];

        snprintf(msg, sizeof(msg),
                 "NANOCOMPAT: run exit=%d saved emergency file\n", code);
        emit(msg);
        return 0;
    }

    {
        char msg[96];

        snprintf(msg, sizeof(msg),
                 "NANOCOMPAT FAIL NANOCOMPAT: run exit=%d expected saved file\n",
                 code);
        emit(msg);
    }
    emit("NANOCOMPAT OUTPUT BEGIN\n");
    emit(out);
    emit("NANOCOMPAT OUTPUT END\n");
    return -1;
}

int main(void)
{
    char out[OUT_CAP];
    char *envp[] = { "PATH=/bin", "TERM=vt100", "HOME=/tmp", 0 };
    char *version_argv[] = { "nano", "--version", 0 };
    char *write_argv[] = { "nano", "--saveonexit", "--nohelp",
                           "/tmp/nano-write.txt", 0 };
    const char *input = "NANOCOMPAT FILE OK\030";

    log_fd = sys_create("/dufs/nano.log");

    emit("NANOCOMPAT BEGIN\n");
    sys_mkdir("/tmp");
    sys_unlink("/tmp/nano-write.txt");
    sys_unlink("/tmp/nano-write.txt.save");

    if (stage_ok("NANOCOMPAT: version", "/bin/nano", version_argv, envp,
                 0, "GNU nano", out, sizeof(out)) != 0)
        goto fail;
    if (nano_write_ok(write_argv, envp, input, out, sizeof(out)) != 0)
        goto fail;

    emit("NANOCOMPAT: write ok\n");
    emit("NANOCOMPAT PASS\n");
    if (log_fd >= 0)
        sys_close(log_fd);
    return 0;

fail:
    emit("NANOCOMPAT FAIL\n");
    if (log_fd >= 0)
        sys_close(log_fd);
    return 1;
}

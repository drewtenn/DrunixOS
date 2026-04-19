/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * tcccompat.c - unattended TinyCC/Linux i386 compatibility runner.
 */

#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/syscall.h"

#define OUT_CAP 8192

static int log_fd = -1;

static const char hello_source[] =
    "static void sys_write(const char *s, unsigned n) {\n"
    "    int r;\n"
    "    __asm__ volatile(\"int $0x80\"\n"
    "        : \"=a\"(r)\n"
    "        : \"a\"(4), \"b\"(1), \"c\"(s), \"d\"(n)\n"
    "        : \"memory\");\n"
    "}\n"
    "static void sys_exit(int code) {\n"
    "    __asm__ volatile(\"int $0x80\"\n"
    "        :: \"a\"(1), \"b\"(code)\n"
    "        : \"memory\");\n"
    "    for (;;)\n"
    "        ;\n"
    "}\n"
    "void _start(void) {\n"
    "    sys_write(\"TCCHELLO OK\\n\", 12);\n"
    "    sys_exit(0);\n"
    "}\n";

static const char multi_main_source[] =
    "static void sys_write(const char *s, unsigned n) {\n"
    "    int r;\n"
    "    __asm__ volatile(\"int $0x80\"\n"
    "        : \"=a\"(r)\n"
    "        : \"a\"(4), \"b\"(1), \"c\"(s), \"d\"(n)\n"
    "        : \"memory\");\n"
    "}\n"
    "static void sys_exit(int code) {\n"
    "    __asm__ volatile(\"int $0x80\"\n"
    "        :: \"a\"(1), \"b\"(code)\n"
    "        : \"memory\");\n"
    "    for (;;)\n"
    "        ;\n"
    "}\n"
    "extern int tcc_multi_value(void);\n"
    "void _start(void) {\n"
    "    if (tcc_multi_value() == 42) {\n"
    "        sys_write(\"TCCMULTI OK\\n\", 12);\n"
    "        sys_exit(0);\n"
    "    }\n"
    "    sys_write(\"TCCMULTI BAD\\n\", 13);\n"
    "    sys_exit(1);\n"
    "}\n";

static const char multi_util_source[] =
    "int tcc_multi_value(void) {\n"
    "    return 42;\n"
    "}\n";

static const char runtime_source[] =
    "#include <stdio.h>\n"
    "#include <string.h>\n"
    "\n"
    "int main(void) {\n"
    "    if (strcmp(\"drunix\", \"drunix\") == 0) {\n"
    "        printf(\"TCCRT OK\\n\");\n"
    "        return 0;\n"
    "    }\n"
    "    printf(\"TCCRT BAD\\n\");\n"
    "    return 1;\n"
    "}\n";

static const char gcc_source[] =
    "int gcc_value(void) {\n"
    "    return 7;\n"
    "}\n";

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

static int write_text_file(const char *path, const char *content)
{
    int fd;
    int len = (int)strlen(content);

    sys_unlink(path);
    fd = sys_create(path);
    if (fd < 0)
        return -1;
    if (sys_fwrite(fd, content, len) != len) {
        sys_close(fd);
        return -1;
    }
    sys_close(fd);
    return 0;
}

static int run_capture(const char *path, char **argv, char **envp,
                       char *out, int out_cap)
{
    int pipefd[2];
    int pid;
    int used = 0;

    if (sys_pipe(pipefd) != 0)
        return 255;

    pid = sys_fork();
    if (pid == 0) {
        sys_dup2(pipefd[1], 1);
        sys_dup2(pipefd[1], 2);
        sys_close(pipefd[0]);
        sys_close(pipefd[1]);
        sys_execve(path, argv, envp);
        sys_write("exec failed\n");
        sys_exit(127);
    }

    if (pid < 0) {
        sys_close(pipefd[0]);
        sys_close(pipefd[1]);
        return 255;
    }

    sys_close(pipefd[1]);
    for (;;) {
        char chunk[256];
        int n = sys_read(pipefd[0], chunk, sizeof(chunk));

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
    sys_close(pipefd[0]);

    return wait_exit_code(sys_waitpid(pid, 0));
}

static int stage_ok(const char *stage, const char *path, char **argv, char **envp,
                    const char *must_contain, char *out, int out_cap)
{
    int code = run_capture(path, argv, envp, out, out_cap);
    int ok = code == 0 && text_contains(out, must_contain);

    if (ok) {
        emit(stage);
        emit(" ok\n");
        return 0;
    }

    emitf("TCCCOMPAT FAIL %s exit=%d expected=0\n", stage, code, 0);
    emit("TCCCOMPAT OUTPUT BEGIN\n");
    emit(out);
    emit("TCCCOMPAT OUTPUT END\n");
    return -1;
}

int main(void)
{
    char out[OUT_CAP];
    char *envp[] = { "PATH=/usr/bin:/i686-linux-musl/bin:/bin", 0 };
    char *version_argv[] = { "tcc", "-v", 0 };
    char *compile_argv[] = { "tcc", "-nostdlib", "-static", "-o", "/tmp/tcchello",
                             "/tmp/tcchello.c", 0 };
    char *run_argv[] = { "/tmp/tcchello", 0 };
    char *multi_compile_argv[] = { "tcc", "-nostdlib", "-static", "-o",
                                   "/tmp/tccmulti", "/tmp/tccmain.c",
                                   "/tmp/tccutil.c", 0 };
    char *multi_run_argv[] = { "/tmp/tccmulti", 0 };
    char *runtime_compile_argv[] = { "tcc", "-nostdlib", "-static",
                                     "-I/usr/include", "-o", "/tmp/tccrt",
                                     "/usr/lib/drunix/crt0.o", "/tmp/tccrt.c",
                                     "/usr/lib/drunix/libc.a", 0 };
    char *runtime_run_argv[] = { "/tmp/tccrt", 0 };
    char *readelf_argv[] = { "readelf", "-h", "/tmp/tccrt", 0 };
    char *objdump_argv[] = { "objdump", "-f", "/tmp/tccrt", 0 };
    char *gcc_as_argv[] = { "as", "--version", 0 };
    char *gcc_compile_argv[] = { "gcc", "-c", "-nostdinc", "-o",
                                 "/tmp/gcchello.o", "/tmp/gcchello.c", 0 };

    log_fd = sys_create("/dufs/tcc.log");

    emit("TCCCOMPAT BEGIN\n");
    sys_mkdir("/tmp");
    sys_unlink("/tmp/tcchello.c");
    sys_unlink("/tmp/tcchello");
    sys_unlink("/tmp/tccmain.c");
    sys_unlink("/tmp/tccutil.c");
    sys_unlink("/tmp/tccmulti");
    sys_unlink("/tmp/tccrt.c");
    sys_unlink("/tmp/tccrt");
    sys_unlink("/tmp/gcchello.c");
    sys_unlink("/tmp/gcchello.o");
    if (write_text_file("/tmp/tcchello.c", hello_source) == 0) {
        emit("TCCCOMPAT: source write ok\n");
    } else {
        emit("TCCCOMPAT: source write fail\n");
        goto fail;
    }

    if (stage_ok("TCCCOMPAT: version", "/bin/tcc", version_argv, envp,
                 "tcc version", out, sizeof(out)) != 0)
        goto fail;
    if (stage_ok("TCCCOMPAT: compile", "/bin/tcc", compile_argv, envp,
                 0, out, sizeof(out)) != 0)
        goto fail;
    if (stage_ok("TCCCOMPAT: run", "/tmp/tcchello", run_argv, envp,
                 "TCCHELLO OK\n", out, sizeof(out)) != 0)
        goto fail;
    if (write_text_file("/tmp/tccmain.c", multi_main_source) == 0 &&
        write_text_file("/tmp/tccutil.c", multi_util_source) == 0) {
        emit("TCCCOMPAT: multi source write ok\n");
    } else {
        emit("TCCCOMPAT: multi source write fail\n");
        goto fail;
    }
    if (stage_ok("TCCCOMPAT: multi compile", "/bin/tcc", multi_compile_argv,
                 envp, 0, out, sizeof(out)) != 0)
        goto fail;
    if (stage_ok("TCCCOMPAT: multi run", "/tmp/tccmulti", multi_run_argv, envp,
                 "TCCMULTI OK\n", out, sizeof(out)) != 0)
        goto fail;
    if (write_text_file("/tmp/tccrt.c", runtime_source) == 0) {
        emit("TCCCOMPAT: runtime source write ok\n");
    } else {
        emit("TCCCOMPAT: runtime source write fail\n");
        goto fail;
    }
    if (stage_ok("TCCCOMPAT: runtime compile", "/bin/tcc",
                 runtime_compile_argv, envp, 0, out, sizeof(out)) != 0)
        goto fail;
    if (stage_ok("TCCCOMPAT: runtime run", "/tmp/tccrt", runtime_run_argv, envp,
                 "TCCRT OK\n", out, sizeof(out)) != 0)
        goto fail;
    if (stage_ok("TCCCOMPAT: readelf", "/bin/readelf", readelf_argv, envp,
                 "ELF Header", out, sizeof(out)) != 0)
        goto fail;
    if (stage_ok("TCCCOMPAT: objdump", "/bin/objdump", objdump_argv, envp,
                 "file format", out, sizeof(out)) != 0)
        goto fail;
    if (write_text_file("/tmp/gcchello.c", gcc_source) == 0) {
        emit("TCCCOMPAT: gcc source write ok\n");
    } else {
        emit("TCCCOMPAT: gcc source write fail\n");
        goto fail;
    }
    emit("TCCCOMPAT: gcc path env ok\n");
    if (stage_ok("TCCCOMPAT: gcc as", "/usr/bin/as", gcc_as_argv,
                 envp, "GNU assembler", out, sizeof(out)) != 0)
        goto fail;
    if (stage_ok("TCCCOMPAT: gcc compile", "/usr/bin/gcc", gcc_compile_argv,
                 envp, 0, out, sizeof(out)) != 0)
        goto fail;

    emit("TCCCOMPAT PASS\n");
    if (log_fd >= 0)
        sys_close(log_fd);
    return 0;

fail:
    emit("TCCCOMPAT FAIL\n");
    if (log_fd >= 0)
        sys_close(log_fd);
    return 1;
}

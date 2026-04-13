/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * env.c — user-space environment listing utility.
 */

#include "lib/stdio.h"
#include "lib/stdlib.h"
#include "lib/string.h"
#include "lib/syscall.h"

static int has_slash(const char *s)
{
    for (; *s; s++)
        if (*s == '/')
            return 1;
    return 0;
}

static int is_file(const char *path)
{
    int fd = sys_open(path);
    if (fd < 0)
        return 0;
    sys_close(fd);
    return 1;
}

static const char *env_value(char **envp, const char *name)
{
    size_t len = strlen(name);

    if (!envp)
        return 0;
    for (int i = 0; envp[i]; i++)
        if (strncmp(envp[i], name, len) == 0 && envp[i][len] == '=')
            return envp[i] + len + 1;
    return 0;
}

static int build_path(const char *dir, int dir_len,
                      const char *name, char *out, int outsz)
{
    int i = 0;

    if (outsz <= 0)
        return -1;
    if (dir_len == 0)
    {
        while (i < outsz - 1 && name[i])
        {
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

static int resolve_command(char **envp, const char *name, char *out, int outsz)
{
    const char *path;
    const char *start;

    if (has_slash(name))
    {
        strncpy(out, name, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
        return is_file(out) ? 0 : -1;
    }

    path = env_value(envp, "PATH");
    if (!path || !*path)
        path = "/bin";

    start = path;
    for (;;)
    {
        const char *end = start;
        char candidate[128];
        int len;

        while (*end && *end != ':')
            end++;
        len = (int)(end - start);

        if (build_path(start, len, name, candidate, sizeof(candidate)) == 0 &&
            is_file(candidate))
        {
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

static int env_count(char **envp)
{
    int n = 0;
    if (envp)
        while (envp[n])
            n++;
    return n;
}

static int env_name_len(const char *entry)
{
    char *eq = strchr(entry, '=');
    return eq ? (int)(eq - entry) : -1;
}

static int env_find(char **envp, const char *entry)
{
    int name_len = env_name_len(entry);

    if (name_len < 0)
        return -1;
    for (int i = 0; envp && envp[i]; i++)
    {
        char *eq = strchr(envp[i], '=');
        if (eq && (int)(eq - envp[i]) == name_len &&
            strncmp(envp[i], entry, (size_t)name_len) == 0)
            return i;
    }
    return -1;
}

static int env_set(char ***envp_io, int *count_io, const char *entry)
{
    int idx;
    char **envp = *envp_io;
    int count = *count_io;

    if (!strchr(entry, '='))
        return -1;

    idx = env_find(envp, entry);
    if (idx >= 0)
    {
        char *copy = strdup(entry);
        if (!copy)
            return -1;
        free(envp[idx]);
        envp[idx] = copy;
        return 0;
    }

    char **new_envp = (char **)realloc(envp, sizeof(char *) * (size_t)(count + 2));
    if (!new_envp)
        return -1;
    envp = new_envp;
    envp[count] = strdup(entry);
    if (!envp[count])
    {
        envp[count] = 0;
        return -1;
    }
    envp[count + 1] = 0;
    *envp_io = envp;
    *count_io = count + 1;
    return 0;
}

static char **env_clone(char **src, int *count_out)
{
    int count = env_count(src);
    char **dst = (char **)malloc(sizeof(char *) * (size_t)(count + 1));

    if (!dst)
        return 0;
    for (int i = 0; i < count; i++)
    {
        dst[i] = strdup(src[i]);
        if (!dst[i])
        {
            for (int j = 0; j < i; j++)
                free(dst[j]);
            free(dst);
            return 0;
        }
    }
    dst[count] = 0;
    *count_out = count;
    return dst;
}

int main(int argc, char **argv)
{
    int first = 1;
    int envc = 0;
    char **envp;

    if (argc > 1 && (!strcmp(argv[1], "-i") || !strcmp(argv[1], "-")))
    {
        envp = (char **)malloc(sizeof(char *));
        if (!envp)
            return 125;
        envp[0] = 0;
        first = 2;
    }
    else
    {
        envp = env_clone(environ, &envc);
        if (!envp)
            return 125;
    }

    while (first < argc && strchr(argv[first], '='))
    {
        if (env_set(&envp, &envc, argv[first]) != 0)
        {
            fprintf(stderr, "env: cannot set: %s\n", argv[first]);
            return 125;
        }
        first++;
    }

    if (first == argc)
    {
        for (int i = 0; envp[i]; i++)
            printf("%s\n", envp[i]);
        return 0;
    }

    char path[128];
    if (resolve_command(envp, argv[first], path, sizeof(path)) != 0)
    {
        fprintf(stderr, "env: not found: %s\n", argv[first]);
        return 127;
    }

    sys_execve(path, &argv[first], argc - first, envp, envc);
    fprintf(stderr, "env: cannot execute: %s\n", argv[first]);
    return 126;
}

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kill.c — user-space signal-sending utility.
 */

#include "stdio.h"
#include "string.h"
#include "syscall.h"

static int parse_number(const char *s, int *out)
{
    int sign = 1;
    int n = 0;

    if (!s || !*s)
        return 0;
    if (*s == '-')
    {
        sign = -1;
        s++;
    }
    else if (*s == '+')
        s++;
    if (!*s)
        return 0;
    while (*s)
    {
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
    int n;

    if (!strcmp(name, "HUP")) return 1;
    if (!strcmp(name, "INT")) return SIGINT;
    if (!strcmp(name, "ILL")) return SIGILL;
    if (!strcmp(name, "TRAP")) return SIGTRAP;
    if (!strcmp(name, "ABRT")) return SIGABRT;
    if (!strcmp(name, "FPE")) return SIGFPE;
    if (!strcmp(name, "KILL")) return SIGKILL;
    if (!strcmp(name, "SEGV")) return SIGSEGV;
    if (!strcmp(name, "PIPE")) return SIGPIPE;
    if (!strcmp(name, "TERM")) return SIGTERM;
    if (!strcmp(name, "CHLD")) return SIGCHLD;
    if (!strcmp(name, "CONT")) return SIGCONT;
    if (!strcmp(name, "STOP")) return SIGSTOP;
    if (!strcmp(name, "TSTP")) return SIGTSTP;
    if (!strncmp(name, "SIG", 3))
        return signal_number(name + 3);
    if (parse_number(name, &n))
        return n;
    return -1;
}

static const char *signal_name(int sig)
{
    switch (sig)
    {
    case 1: return "HUP";
    case SIGINT: return "INT";
    case SIGILL: return "ILL";
    case SIGTRAP: return "TRAP";
    case SIGABRT: return "ABRT";
    case SIGFPE: return "FPE";
    case SIGKILL: return "KILL";
    case SIGSEGV: return "SEGV";
    case SIGPIPE: return "PIPE";
    case SIGTERM: return "TERM";
    case SIGCHLD: return "CHLD";
    case SIGCONT: return "CONT";
    case SIGSTOP: return "STOP";
    case SIGTSTP: return "TSTP";
    default: return 0;
    }
}

int main(int argc, char **argv)
{
    int sig = SIGTERM;
    int first_pid = 1;
    int rc = 0;

    if (argc < 2)
    {
        fprintf(stderr, "usage: kill [-s signal|-signal] pid...\n");
        return 2;
    }

    if (!strcmp(argv[1], "-l"))
    {
        if (argc == 2)
        {
            printf("HUP INT ILL TRAP ABRT FPE KILL SEGV PIPE TERM CHLD CONT STOP TSTP\n");
            return 0;
        }
        for (int i = 2; i < argc; i++)
        {
            int n;
            const char *name;
            if (!parse_number(argv[i], &n))
                return 2;
            name = signal_name(n);
            if (name)
                printf("%s\n", name);
            else
                rc = 1;
        }
        return rc;
    }

    if (!strcmp(argv[1], "-s"))
    {
        if (argc < 4)
        {
            fprintf(stderr, "usage: kill -s signal pid...\n");
            return 2;
        }
        sig = signal_number(argv[2]);
        first_pid = 3;
    }
    else if (argv[1][0] == '-' && argv[1][1] != '\0')
    {
        sig = signal_number(argv[1] + 1);
        first_pid = 2;
    }

    if (sig <= 0)
    {
        fprintf(stderr, "kill: invalid signal\n");
        return 2;
    }

    for (int i = first_pid; i < argc; i++)
    {
        int pid;
        if (!parse_number(argv[i], &pid) || pid == 0)
        {
            fprintf(stderr, "kill: invalid pid: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        if (sys_kill(pid, sig) != 0)
        {
            fprintf(stderr, "kill: failed: %s\n", argv[i]);
            rc = 1;
        }
    }

    return rc;
}

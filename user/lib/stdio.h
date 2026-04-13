/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif

/*
 * stdio.h — FILE* streams and the printf family, built on top of the
 * existing syscall wrappers and heap allocator.
 *
 * The three standard streams are static FILE structs in stdio.c that wrap
 * the kernel-provided file descriptors:
 *
 *   stdin  → fd 0 (FD_TYPE_TTY in the kernel, line-disciplined keyboard)
 *   stdout → fd 1 (FD_TYPE_STDOUT, routed to VGA by SYS_FWRITE, but will
 *                  transparently follow a dup2 into a pipe or file)
 *   stderr → fd 2 (FD_TYPE_STDOUT, same as stdout by default)
 *
 * Because the kernel already dispatches SYS_FWRITE on the per-process fd
 * type, printf going through sys_fwrite(stdout->fd, ...) flows through a
 * shell pipeline with no special cases in libc.
 */

#define EOF (-1)

/* FILE flag bits. */
#define _IO_READ   (1u << 0)
#define _IO_WRITE  (1u << 1)
#define _IO_EOF    (1u << 2)
#define _IO_ERR    (1u << 3)
/* Buffering bits reserved for a future line/full-buffered implementation. */
#define _IO_NBF    (1u << 4)
#define _IO_LBF    (1u << 5)
#define _IO_FBF    (1u << 6)

typedef struct FILE {
    int          fd;     /* kernel file descriptor */
    unsigned int flags;  /* _IO_* bits above */
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Character I/O on the standard streams. */
int  putchar(int c);
int  puts(const char *s);
int  getchar(void);

/* Character / line I/O on an arbitrary stream. */
int  fputc(int c, FILE *f);
int  fputs(const char *s, FILE *f);
int  fgetc(FILE *f);
char *fgets(char *buf, int n, FILE *f);
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f);
ssize_t getline(char **lineptr, size_t *n, FILE *f);

/* Block I/O. */
size_t fread (void *buf, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t nmemb, FILE *f);

/* Stream lifetime.
 * mode is a single character: "r" = read (sys_open), "w" = write (sys_create).
 * No "a", no "+" — the underlying filesystem does not yet support them.
 */
FILE *fopen (const char *path, const char *mode);
int   fclose(FILE *f);
int   fflush(FILE *f);   /* no-op until buffering lands */

/* The printf family. */
int printf  (const char *fmt, ...);
int fprintf (FILE *f, const char *fmt, ...);
int sprintf (char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t sz, const char *fmt, ...);
int vfprintf (FILE *f, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t sz, const char *fmt, va_list ap);

#endif

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * stdio.c — FILE* streams and the printf family.
 *
 * The printf engine is centralised in vfprintf_core(): every other variant
 * is a thin wrapper that configures a pf_sink_t and calls it.  The sink
 * abstracts "where do the formatted bytes go" — either into a caller-
 * supplied buffer (snprintf/sprintf) or into a small chunk buffer that
 * flushes to a stream fd via sys_fwrite whenever it fills up (printf/
 * fprintf).  Keeping the format walker single-source guarantees that every
 * printf variant supports exactly the same format specifiers.
 *
 * The three standard streams live as static FILE instances in this file.
 * They wrap the per-process file descriptors the kernel pre-wires in
 * process_create():
 *
 *   stdin  → fd 0 (FD_TYPE_TTY, keyboard via TTY line discipline)
 *   stdout → fd 1 (FD_TYPE_STDOUT, VGA — but follows dup2 into a pipe)
 *   stderr → fd 2 (FD_TYPE_STDOUT, same as stdout initially)
 *
 * Because the kernel SYS_WRITE path already dispatches on fd type (VGA,
 * pipe buffer, or filesystem write), a printf built on sys_fwrite(1, ...)
 * transparently flows through a shell pipeline with zero special cases.
 */

#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "syscall.h"

#include <stdint.h>
#include <stdarg.h>

/* ── Standard streams ─────────────────────────────────────────────────── */

static FILE _stdin  = { 0, _IO_READ  | _IO_LBF };
static FILE _stdout = { 1, _IO_WRITE | _IO_LBF };
static FILE _stderr = { 2, _IO_WRITE | _IO_NBF };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

/* ── printf sink ──────────────────────────────────────────────────────── */

/*
 * A sink collects formatted output.  Two modes:
 *
 *   fd >= 0 ("stream mode"):
 *     buf[]/cap is a local chunk buffer owned by the caller.  When the
 *     chunk fills, pf_put flushes it with sys_fwrite(fd, ...) and resets
 *     pos to 0.  pf_flush() at the end of the format walk emits any
 *     residual bytes.  Used by printf/fprintf/vfprintf.
 *
 *   fd < 0 ("buffer mode"):
 *     buf[]/cap is the caller's destination buffer.  pf_put appends to
 *     it until cap-1 bytes have been written, at which point further
 *     bytes are counted but discarded (C99 snprintf semantics — the
 *     return value is the would-have-been length so callers can size a
 *     second attempt).  pf_flush() writes the trailing NUL.
 */
typedef struct {
    char  *buf;
    size_t cap;
    size_t pos;
    int    total;   /* chars that would have been emitted (printf return) */
    int    fd;      /* -1 for buffer mode, >= 0 for stream mode */
} pf_sink_t;

static void pf_put(pf_sink_t *s, char c)
{
    s->total++;
    if (s->fd >= 0) {
        s->buf[s->pos++] = c;
        if (s->pos == s->cap) {
            sys_fwrite(s->fd, s->buf, (int)s->pos);
            s->pos = 0;
        }
    } else {
        /* Reserve the last byte of the destination buffer for the NUL. */
        if (s->cap > 0 && s->pos + 1 < s->cap)
            s->buf[s->pos++] = c;
    }
}

static void pf_flush(pf_sink_t *s)
{
    if (s->fd >= 0) {
        if (s->pos > 0) {
            sys_fwrite(s->fd, s->buf, (int)s->pos);
            s->pos = 0;
        }
    } else if (s->cap > 0) {
        s->buf[s->pos] = '\0';
    }
}

/* ── printf format walker ─────────────────────────────────────────────── */

/*
 * Render an unsigned integer `v` into `tmp` in base `base` (2..16), digits
 * reversed (least significant first).  Returns the number of digits
 * written.  `upper` selects uppercase hex digits A-F when nonzero.
 */
static int utoa_rev(unsigned int v, unsigned int base, int upper, char *tmp)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; return n; }
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    return n;
}

/*
 * vfprintf_core: the heart of every printf variant.  Walks fmt one
 * character at a time, emitting literal bytes to the sink and, on '%',
 * parsing a conversion specifier of the form
 *
 *     %[-0][width][.precision]<conv>
 *
 * Supported conversions: d i u x X o c s p %
 * Not supported: floating-point, h/hh/l/ll/z length modifiers, %n, *-width,
 * positional arguments.
 */
static int vfprintf_core(pf_sink_t *s, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            pf_put(s, *fmt++);
            continue;
        }
        fmt++;                 /* skip '%' */

        /* --- Flags --- */
        int left_just = 0;
        int zero_pad  = 0;
        for (;;) {
            if      (*fmt == '-') { left_just = 1; fmt++; }
            else if (*fmt == '0') { zero_pad  = 1; fmt++; }
            else break;
        }

        /* --- Width (decimal literal only; '*' not supported) --- */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* --- Precision --- */
        int precision = -1;   /* -1 means "not specified" */
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* --- Render the value into `body` without padding or sign --- */
        char       body_buf[32];
        const char *body = body_buf;
        int         body_len = 0;
        char        sign_ch  = 0;     /* '-' if a negative %d/%i */
        int         is_num   = 0;     /* numeric conversions honour zero_pad */
        const char *prefix   = 0;     /* "0x" for %p; NUL otherwise */
        int         prefix_len = 0;

        char conv = *fmt;
        if (conv == '\0') break;      /* stray '%' at end-of-string */
        fmt++;

        switch (conv) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            unsigned int u;
            if (v < 0) { sign_ch = '-'; u = 0u - (unsigned int)v; }
            else       { u = (unsigned int)v; }

            char tmp[16];
            int tlen = utoa_rev(u, 10, 0, tmp);
            int digits = tlen;
            if (precision >= 0 && precision > digits) digits = precision;

            /* Precision 0 with value 0 → produce no digits. */
            if (precision == 0 && tlen == 1 && tmp[0] == '0') {
                body_len = 0;
            } else {
                int j = 0;
                for (int i = 0; i < digits - tlen; i++) body_buf[j++] = '0';
                for (int i = tlen - 1; i >= 0; i--) body_buf[j++] = tmp[i];
                body_len = j;
            }
            is_num = 1;
            break;
        }
        case 'u': {
            unsigned int u = va_arg(ap, unsigned int);
            char tmp[16];
            int tlen = utoa_rev(u, 10, 0, tmp);
            int digits = tlen;
            if (precision >= 0 && precision > digits) digits = precision;
            if (precision == 0 && tlen == 1 && tmp[0] == '0') {
                body_len = 0;
            } else {
                int j = 0;
                for (int i = 0; i < digits - tlen; i++) body_buf[j++] = '0';
                for (int i = tlen - 1; i >= 0; i--) body_buf[j++] = tmp[i];
                body_len = j;
            }
            is_num = 1;
            break;
        }
        case 'x':
        case 'X': {
            unsigned int u = va_arg(ap, unsigned int);
            char tmp[16];
            int tlen = utoa_rev(u, 16, conv == 'X', tmp);
            int digits = tlen;
            if (precision >= 0 && precision > digits) digits = precision;
            if (precision == 0 && tlen == 1 && tmp[0] == '0') {
                body_len = 0;
            } else {
                int j = 0;
                for (int i = 0; i < digits - tlen; i++) body_buf[j++] = '0';
                for (int i = tlen - 1; i >= 0; i--) body_buf[j++] = tmp[i];
                body_len = j;
            }
            is_num = 1;
            break;
        }
        case 'o': {
            unsigned int u = va_arg(ap, unsigned int);
            char tmp[16];
            int tlen = utoa_rev(u, 8, 0, tmp);
            int digits = tlen;
            if (precision >= 0 && precision > digits) digits = precision;
            if (precision == 0 && tlen == 1 && tmp[0] == '0') {
                body_len = 0;
            } else {
                int j = 0;
                for (int i = 0; i < digits - tlen; i++) body_buf[j++] = '0';
                for (int i = tlen - 1; i >= 0; i--) body_buf[j++] = tmp[i];
                body_len = j;
            }
            is_num = 1;
            break;
        }
        case 'p': {
            /* %p: always "0x" + 8 hex digits, zero-padded.  Ignores
             * flags, width, and precision for predictability. */
            unsigned int u = (unsigned int)(uintptr_t)va_arg(ap, void *);
            char tmp[16];
            int tlen = utoa_rev(u, 16, 0, tmp);
            int j = 0;
            for (int i = 0; i < 8 - tlen; i++) body_buf[j++] = '0';
            for (int i = tlen - 1; i >= 0; i--) body_buf[j++] = tmp[i];
            body_len = j;
            prefix = "0x";
            prefix_len = 2;
            /* Disable width/flags for %p. */
            width = 0;
            left_just = 0;
            zero_pad = 0;
            break;
        }
        case 'c': {
            body_buf[0] = (char)va_arg(ap, int);
            body_len = 1;
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int slen = 0;
            if (precision >= 0) {
                while (slen < precision && str[slen]) slen++;
            } else {
                slen = (int)strlen(str);
            }
            body = str;
            body_len = slen;
            break;
        }
        case '%': {
            body_buf[0] = '%';
            body_len = 1;
            break;
        }
        default: {
            /* Unknown specifier: echo it literally so bugs are visible. */
            body_buf[0] = '%';
            body_buf[1] = conv;
            body_len = 2;
            break;
        }
        }

        /* --- Compose the field: [sign][prefix][pad][body] or [body][pad] --- */
        int total_len = body_len + (sign_ch ? 1 : 0) + prefix_len;
        int pad = width > total_len ? width - total_len : 0;

        /* Zero-pad is only honoured for numeric conversions with no
         * explicit precision (C99 §7.19.6.1). */
        int use_zero = (zero_pad && !left_just && is_num && precision < 0);

        if (left_just) {
            if (sign_ch) pf_put(s, sign_ch);
            for (int i = 0; i < prefix_len; i++) pf_put(s, prefix[i]);
            for (int i = 0; i < body_len; i++)   pf_put(s, body[i]);
            for (int i = 0; i < pad; i++)        pf_put(s, ' ');
        } else if (use_zero) {
            if (sign_ch) pf_put(s, sign_ch);
            for (int i = 0; i < prefix_len; i++) pf_put(s, prefix[i]);
            for (int i = 0; i < pad; i++)        pf_put(s, '0');
            for (int i = 0; i < body_len; i++)   pf_put(s, body[i]);
        } else {
            for (int i = 0; i < pad; i++)        pf_put(s, ' ');
            if (sign_ch) pf_put(s, sign_ch);
            for (int i = 0; i < prefix_len; i++) pf_put(s, prefix[i]);
            for (int i = 0; i < body_len; i++)   pf_put(s, body[i]);
        }
    }

    pf_flush(s);
    return s->total;
}

/* ── Public printf family ─────────────────────────────────────────────── */

int vsnprintf(char *buf, size_t sz, const char *fmt, va_list ap)
{
    pf_sink_t s;
    s.buf   = buf;
    s.cap   = sz;
    s.pos   = 0;
    s.total = 0;
    s.fd    = -1;
    return vfprintf_core(&s, fmt, ap);
}

int snprintf(char *buf, size_t sz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    /* 1 GB cap — "unbounded" for anything a user program will ever print. */
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (!f || !(f->flags & _IO_WRITE)) return -1;

    char chunk[512];
    pf_sink_t s;
    s.buf   = chunk;
    s.cap   = sizeof(chunk);
    s.pos   = 0;
    s.total = 0;
    s.fd    = f->fd;
    return vfprintf_core(&s, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ── Character I/O ────────────────────────────────────────────────────── */

int fputc(int c, FILE *f)
{
    if (!f || !(f->flags & _IO_WRITE)) return EOF;
    char ch = (char)c;
    int n = sys_fwrite(f->fd, &ch, 1);
    return n == 1 ? (int)(unsigned char)ch : EOF;
}

int fputs(const char *s, FILE *f)
{
    if (!f || !(f->flags & _IO_WRITE)) return EOF;
    int len = (int)strlen(s);
    int n = sys_fwrite(f->fd, s, len);
    return n == len ? 0 : EOF;
}

int putchar(int c) { return fputc(c, stdout); }

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

int fgetc(FILE *f)
{
    if (!f || !(f->flags & _IO_READ)) return EOF;
    unsigned char ch;
    int n = sys_read(f->fd, (char *)&ch, 1);
    if (n < 0) {
        f->flags |= _IO_ERR;
        return EOF;
    }
    if (n == 0) {
        f->flags |= _IO_EOF;
        return EOF;
    }
    return (int)ch;
}

int getchar(void) { return fgetc(stdin); }

char *fgets(char *buf, int n, FILE *f)
{
    if (!buf || n <= 0 || !f || !(f->flags & _IO_READ)) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return 0;
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f)
{
    size_t pos = 0;

    if (!lineptr || !n || !f || !(f->flags & _IO_READ))
        return -1;

    if (!*lineptr || *n == 0)
    {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr)
        {
            *n = 0;
            return -1;
        }
    }

    for (;;)
    {
        int c = fgetc(f);
        if (c == EOF)
        {
            if (pos == 0)
                return -1;
            break;
        }

        if (pos + 1 >= *n)
        {
            size_t new_cap = *n * 2;
            char *new_buf = (char *)realloc(*lineptr, new_cap);
            if (!new_buf)
                return -1;
            *lineptr = new_buf;
            *n = new_cap;
        }

        (*lineptr)[pos++] = (char)c;
        if (c == delim)
            break;
    }

    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

ssize_t getline(char **lineptr, size_t *n, FILE *f)
{
    return getdelim(lineptr, n, '\n', f);
}

/* ── Block I/O ────────────────────────────────────────────────────────── */

size_t fwrite(const void *buf, size_t size, size_t nmemb, FILE *f)
{
    if (!f || !(f->flags & _IO_WRITE) || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    int n = sys_fwrite(f->fd, (const char *)buf, (int)total);
    if (n <= 0) return 0;
    return (size_t)n / size;
}

size_t fread(void *buf, size_t size, size_t nmemb, FILE *f)
{
    if (!f || !(f->flags & _IO_READ) || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    int n = sys_read(f->fd, (char *)buf, (int)total);
    if (n <= 0) {
        if (n == 0) f->flags |= _IO_EOF;
        return 0;
    }
    return (size_t)n / size;
}

/* ── Stream lifetime ─────────────────────────────────────────────────── */

FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode) return 0;

    int fd = -1;
    unsigned int flags = 0;

    if (mode[0] == 'r' && mode[1] == '\0') {
        fd = sys_open(path);
        flags = _IO_READ;
    } else if (mode[0] == 'w' && mode[1] == '\0') {
        fd = sys_create(path);
        flags = _IO_WRITE;
    } else {
        return 0;   /* unsupported mode */
    }

    if (fd < 0) return 0;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { sys_close(fd); return 0; }
    f->fd    = fd;
    f->flags = flags;
    return f;
}

int fclose(FILE *f)
{
    if (!f) return EOF;
    /* Never close the standard streams' fds — they're owned by the kernel. */
    if (f == &_stdin || f == &_stdout || f == &_stderr) return 0;

    int r = sys_close(f->fd);
    free(f);
    return r == 0 ? 0 : EOF;
}

int fflush(FILE *f)
{
    /* Streams are currently unbuffered; nothing to do.  The function exists
     * so callers that will be buffered later do not need to be rewritten. */
    (void)f;
    return 0;
}

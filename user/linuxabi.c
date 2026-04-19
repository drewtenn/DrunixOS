/*
 * linuxabi.c - direct Linux i386 syscall semantics checks for Drunix.
 *
 * Built as a static Linux/i386 ELF with i486-linux-musl-gcc.  The tests use
 * raw int 0x80 calls so they validate Drunix's Linux ABI return values instead
 * of libc wrapper behavior.
 */

#include <stdint.h>

#define SYS_EXIT        1
#define SYS_FORK        2
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_WAITPID     7
#define SYS_CREAT       8
#define SYS_UNLINK      10
#define SYS_CHDIR       12
#define SYS_CHMOD       15
#define SYS_LCHOWN      16
#define SYS_LSEEK       19
#define SYS_GETPID      20
#define SYS_ACCESS      33
#define SYS_SYNC        36
#define SYS_KILL        37
#define SYS_RENAME      38
#define SYS_MKDIR       39
#define SYS_RMDIR       40
#define SYS_DUP         41
#define SYS_PIPE        42
#define SYS_BRK         45
#define SYS_IOCTL       54
#define SYS_SETPGID     57
#define SYS_UMASK       60
#define SYS_DUP2        63
#define SYS_GETPPID     64
#define SYS_SETSID      66
#define SYS_SIGACTION   67
#define SYS_GETRUSAGE   77
#define SYS_GETTIMEOFDAY 78
#define SYS_READLINK    85
#define SYS_MMAP        90
#define SYS_MUNMAP      91
#define SYS_GETPRIORITY 96
#define SYS_SETPRIORITY 97
#define SYS_WAIT4       114
#define SYS_SYSINFO     116
#define SYS_CLONE       120
#define SYS_UNAME       122
#define SYS_MPROTECT   125
#define SYS_SIGPROCMASK 126
#define SYS_GETPGID     132
#define SYS__LLSEEK    140
#define SYS_GETDENTS   141
#define SYS__NEWSELECT 142
#define SYS_READV      145
#define SYS_WRITEV     146
#define SYS_GETSID      147
#define SYS_YIELD       158
#define SYS_NANOSLEEP   162
#define SYS_POLL       168
#define SYS_RT_SIGACTION 174
#define SYS_RT_SIGPROCMASK 175
#define SYS_GETCWD      183
#define SYS_VFORK      190
#define SYS_MMAP2      192
#define SYS_TRUNCATE64 193
#define SYS_FTRUNCATE64 194
#define SYS_STAT       106
#define SYS_LSTAT64    107
#define SYS_STAT64      195
#define SYS_FSTAT64     197
#define SYS_GETUID32    199
#define SYS_GETGID32    200
#define SYS_GETEUID32   201
#define SYS_GETEGID32   202
#define SYS_CHOWN32     212
#define SYS_SETUID32    213
#define SYS_SETGID32    214
#define SYS_GETDENTS64  220
#define SYS_FCNTL64     221
#define SYS_GETTID      224
#define SYS_SENDFILE64  239
#define SYS_SCHED_GETAFFINITY 242
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP  252
#define SYS_SET_TID_ADDRESS 258
#define SYS_CLOCK_GETTIME 265
#define SYS_STATFS64    268
#define SYS_FSTATFS64   269
#define SYS_OPENAT      295
#define SYS_MKDIRAT     296
#define SYS_FCHOWNAT    298
#define SYS_FUTIMESAT   299
#define SYS_FSTATAT64   300
#define SYS_UNLINKAT    301
#define SYS_RENAMEAT    302
#define SYS_LINKAT      303
#define SYS_SYMLINKAT   304
#define SYS_READLINKAT  305
#define SYS_FCHMODAT    306
#define SYS_FACCESSAT   307
#define SYS_UTIMENSAT   320
#define SYS_PIPE2       331
#define SYS_PRLIMIT64   340
#define SYS_STATX       383
#define SYS_CLOCK_GETTIME64 403

#define O_WRONLY 01
#define O_RDWR   02
#define O_ACCMODE 03
#define O_CREAT  0100
#define O_EXCL   0200
#define O_TRUNC  01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000
#define O_DIRECTORY 0200000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_THREAD         0x00010000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u

#define AT_FDCWD ((long)-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EMPTY_PATH 0x1000

#define S_IFMT  00170000
#define S_IFDIR 0040000
#define S_IFLNK 0120000

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLNVAL 0x0020

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGWINSZ 0x5413
#define FIONREAD 0x541B

#define ENOENT 2
#define ESRCH 3
#define EAGAIN 11
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ELOOP 40

#define SIG_DFL 0
#define SIG_IGN 1
#define SIGKILL 9
#define SIGCHLD 17
#define SIGTERM 15
#define SIGSTOP 19

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define WNOHANG 1

#define RLIMIT_STACK 3

static int log_fd = -1;
static int passed = 0;
static int total = 0;

static long sc0(long n)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

static long sc1(long n, long a)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static long sc2(long n, long a, long b)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static long sc3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static long sc4(long n, long a, long b, long c, long d)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static long sc5(long n, long a, long b, long c, long d, long e)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
    return r;
}

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    __asm__ volatile(
        "push %%ebp\n\t"
        "mov %[arg6], %%ebp\n\t"
        "int $0x80\n\t"
        "pop %%ebp"
        : "=a"(r)
        : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e),
          [arg6] "r"(f)
        : "memory");
    return r;
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s && s[n])
        n++;
    return n;
}

static int streq(const char *a, const char *b)
{
    unsigned i = 0;
    while (a[i] && b[i] && a[i] == b[i])
        i++;
    return a[i] == b[i];
}

static int memeq(const char *a, const char *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static void emit_raw(const char *s)
{
    unsigned n = slen(s);
    sc3(SYS_WRITE, 1, (long)s, n);
    if (log_fd >= 0)
        sc3(SYS_WRITE, log_fd, (long)s, n);
}

static void emit_uint(unsigned value)
{
    char buf[11];
    unsigned i = sizeof(buf);

    if (value == 0) {
        emit_raw("0");
        return;
    }
    while (value && i > 0) {
        buf[--i] = (char)('0' + (value % 10));
        value /= 10;
    }
    sc3(SYS_WRITE, 1, (long)&buf[i], sizeof(buf) - i);
    if (log_fd >= 0)
        sc3(SYS_WRITE, log_fd, (long)&buf[i], sizeof(buf) - i);
}

static void pass(const char *name)
{
    total++;
    passed++;
    emit_raw("LINUXABI PASS ");
    emit_raw(name);
    emit_raw("\n");
}

static void fail(const char *name, long got, long want)
{
    total++;
    emit_raw("LINUXABI FAIL ");
    emit_raw(name);
    emit_raw(" got=");
    if (got < 0) {
        emit_raw("-");
        emit_uint((unsigned)-got);
    } else {
        emit_uint((unsigned)got);
    }
    emit_raw(" want=");
    if (want < 0) {
        emit_raw("-");
        emit_uint((unsigned)-want);
    } else {
        emit_uint((unsigned)want);
    }
    emit_raw("\n");
}

static void check_eq(const char *name, long got, long want)
{
    if (got == want)
        pass(name);
    else
        fail(name, got, want);
}

static void check_ok(const char *name, long got)
{
    if (got >= 0)
        pass(name);
    else
        fail(name, got, 0);
}

static uint32_t get_u32(const unsigned char *buf, unsigned off)
{
    return (uint32_t)buf[off] |
           ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

static uint64_t get_u64(const unsigned char *buf, unsigned off)
{
    return (uint64_t)get_u32(buf, off) |
           ((uint64_t)get_u32(buf, off + 4) << 32);
}

static void put_u32(unsigned char *buf, unsigned off, uint32_t value)
{
    buf[off + 0] = (unsigned char)(value & 0xFFu);
    buf[off + 1] = (unsigned char)((value >> 8) & 0xFFu);
    buf[off + 2] = (unsigned char)((value >> 16) & 0xFFu);
    buf[off + 3] = (unsigned char)((value >> 24) & 0xFFu);
}

static void put_u16(unsigned char *buf, unsigned off, uint32_t value)
{
    buf[off + 0] = (unsigned char)(value & 0xFFu);
    buf[off + 1] = (unsigned char)((value >> 8) & 0xFFu);
}

static int is_linux_error(long value)
{
    return value < 0 && value >= -4095;
}

static int dirents64_contains(const unsigned char *buf, long len, const char *name)
{
    long pos = 0;

    while (pos + 19 < len) {
        unsigned reclen = (unsigned)buf[pos + 16] |
                          ((unsigned)buf[pos + 17] << 8);
        const char *entry = (const char *)&buf[pos + 19];

        if (reclen < 20 || pos + reclen > len)
            return 0;
        if (streq(entry, name))
            return 1;
        pos += reclen;
    }
    return 0;
}

static int dirents_contains(const unsigned char *buf, long len, const char *name)
{
    long pos = 0;

    while (pos + 12 < len) {
        unsigned reclen = (unsigned)buf[pos + 8] |
                          ((unsigned)buf[pos + 9] << 8);
        const char *entry = (const char *)&buf[pos + 10];

        if (reclen < 12 || pos + reclen > len)
            return 0;
        if (streq(entry, name))
            return 1;
        pos += reclen;
    }
    return 0;
}

static void test_identity(void)
{
    long pid = sc0(SYS_GETPID);
    long ppid = sc0(SYS_GETPPID);
    uint32_t tid_slot = 0;
    uint32_t user_desc[4];

    check_ok("getpid returns positive pid", pid > 0 ? pid : -1);
    check_eq("getpid is stable", sc0(SYS_GETPID), pid);
    check_eq("gettid matches getpid for single-thread process", sc0(SYS_GETTID), pid);
    check_ok("gettid returns positive tid", sc0(SYS_GETTID));
    check_ok("getppid returns a pid", ppid);
    check_eq("getppid is stable", sc0(SYS_GETPPID), ppid);
    check_eq("getuid32 reports root", sc0(SYS_GETUID32), 0);
    check_eq("geteuid32 reports root", sc0(SYS_GETEUID32), 0);
    check_eq("getgid32 reports root", sc0(SYS_GETGID32), 0);
    check_eq("getegid32 reports root", sc0(SYS_GETEGID32), 0);
    check_eq("setuid32 accepts root", sc1(SYS_SETUID32, 0), 0);
    check_eq("setgid32 accepts root", sc1(SYS_SETGID32, 0), 0);
    check_eq("setuid32 rejects nonroot", sc1(SYS_SETUID32, 1), -1);
    check_eq("setgid32 rejects nonroot", sc1(SYS_SETGID32, 1), -1);
    check_eq("getuid32 remains root after rejected setuid", sc0(SYS_GETUID32), 0);
    check_eq("set_tid_address returns current tid", sc1(SYS_SET_TID_ADDRESS, (long)&tid_slot), pid);
    check_eq("set_tid_address accepts null pointer", sc1(SYS_SET_TID_ADDRESS, 0), pid);
    check_eq("clone rejects CLONE_SIGHAND without CLONE_VM",
             sc5(SYS_CLONE, CLONE_SIGHAND | SIGCHLD, 0, 0, 0, 0), -1);
    check_eq("clone rejects CLONE_THREAD without CLONE_SIGHAND",
             sc5(SYS_CLONE, CLONE_THREAD | CLONE_VM | SIGCHLD, 0, 0, 0, 0), -1);

    user_desc[0] = 0xFFFFFFFFu;
    user_desc[1] = (uint32_t)&tid_slot;
    user_desc[2] = 0xFFFFFu;
    user_desc[3] = 0x51u;
    check_eq("set_thread_area accepts TLS descriptor", sc1(SYS_SET_THREAD_AREA, (long)user_desc), 0);
    check_eq("set_thread_area assigns Linux TLS entry", user_desc[0], 7);
    user_desc[0] = 99;
    user_desc[1] = (uint32_t)&tid_slot;
    user_desc[2] = 0xFFFFFu;
    user_desc[3] = 0x51u;
    check_eq("set_thread_area rejects invalid entry", sc1(SYS_SET_THREAD_AREA, (long)user_desc), -1);
    user_desc[0] = 0xFFFFFFFFu;
    user_desc[3] = 0x59u;
    check_eq("set_thread_area rejects expand-down segment", sc1(SYS_SET_THREAD_AREA, (long)user_desc), -1);
    user_desc[0] = 7;
    user_desc[3] = 0x20u;
    check_eq("set_thread_area disables TLS descriptor", sc1(SYS_SET_THREAD_AREA, (long)user_desc), 0);
    user_desc[0] = 0xFFFFFFFFu;
    user_desc[1] = (uint32_t)&tid_slot;
    user_desc[2] = 0xFFFFFu;
    user_desc[3] = 0x51u;
    sc1(SYS_SET_THREAD_AREA, (long)user_desc);
}

static void test_system_info(void)
{
    unsigned char uts[390];
    unsigned char info[64];
    unsigned char mask[4];
    long n;

    check_eq("uname succeeds", sc1(SYS_UNAME, (long)uts), 0);
    if (streq((const char *)uts, "Drunix"))
        pass("uname reports Drunix sysname");
    else
        fail("uname reports Drunix sysname", 0, 1);
    if (streq((const char *)(uts + 65), "drunix"))
        pass("uname reports Drunix nodename");
    else
        fail("uname reports Drunix nodename", 0, 1);
    if (streq((const char *)(uts + 130), "0.1"))
        pass("uname reports Drunix release");
    else
        fail("uname reports Drunix release", 0, 1);
    if (streq((const char *)(uts + 195), "Drunix Linux i386 ABI"))
        pass("uname reports Drunix version");
    else
        fail("uname reports Drunix version", 0, 1);
    if (streq((const char *)(uts + 260), "i486"))
        pass("uname reports i486 machine");
    else
        fail("uname reports i486 machine", 0, 1);
    if (streq((const char *)(uts + 325), "drunix.local"))
        pass("uname reports Drunix domain");
    else
        fail("uname reports Drunix domain", 0, 1);
    check_eq("sysinfo succeeds", sc1(SYS_SYSINFO, (long)info), 0);
    check_eq("sysinfo reports mem_unit one", (long)get_u32(info, 52), 1);
    check_eq("sysinfo reports totalram", (long)get_u32(info, 16), 16 * 1024 * 1024);
    check_eq("sysinfo reports freeram", (long)get_u32(info, 20), 8 * 1024 * 1024);
    check_ok("sysinfo reports process count", get_u32(info, 40) & 0xFFFFu);
    n = sc3(SYS_SCHED_GETAFFINITY, 0, sizeof(mask), (long)mask);
    check_eq("sched_getaffinity returns mask size", n, sizeof(mask));
    check_eq("sched_getaffinity enables cpu zero", mask[0] & 1u, 1);
    check_eq("sched_getaffinity rejects small mask", sc3(SYS_SCHED_GETAFFINITY, 0, 1, (long)mask), -EINVAL);
}

static void test_process_controls(void)
{
    long pid = sc0(SYS_GETPID);
    uint32_t req[2];
    unsigned char rusage[72];

    check_eq("getpriority returns default priority", sc2(SYS_GETPRIORITY, 0, 0), 0);
    check_eq("setpriority accepts root priority", sc3(SYS_SETPRIORITY, 0, 0, 0), 0);
    for (unsigned i = 0; i < sizeof(rusage); i++)
        rusage[i] = 0xFF;
    check_eq("getrusage self succeeds", sc2(SYS_GETRUSAGE, 0, (long)rusage), 0);
    check_eq("getrusage self user seconds zero", (long)get_u32(rusage, 0), 0);
    check_eq("getrusage children succeeds", sc2(SYS_GETRUSAGE, -1, (long)rusage), 0);
    check_eq("getrusage rejects invalid who", sc2(SYS_GETRUSAGE, 99, (long)rusage), -EINVAL);
    check_eq("sync succeeds", sc0(SYS_SYNC), 0);
    check_eq("umask returns default mask", sc1(SYS_UMASK, 077), 022);
    check_eq("umask stores previous mask", sc1(SYS_UMASK, 022), 077);
    check_eq("umask returns previous restored mask", sc1(SYS_UMASK, 01234), 022);
    check_eq("umask stores only permission bits", sc1(SYS_UMASK, 022), 0234);
    check_ok("getpgid self succeeds", sc1(SYS_GETPGID, 0));
    check_ok("getsid self succeeds", sc1(SYS_GETSID, 0));
    check_eq("setsid returns current pid", sc0(SYS_SETSID), pid);
    check_eq("getpgid after setsid returns current pid", sc1(SYS_GETPGID, 0), pid);
    check_eq("getsid after setsid returns current pid", sc1(SYS_GETSID, 0), pid);
    check_eq("getpgid missing pid fails", sc1(SYS_GETPGID, 99999), -1);
    check_eq("getsid missing pid returns ESRCH", sc1(SYS_GETSID, 99999), -ESRCH);
    check_eq("setpgid self no-op succeeds", sc2(SYS_SETPGID, 0, 0), 0);
    check_eq("setpgid session leader other pgid fails", sc2(SYS_SETPGID, 0, pid + 1), -1);
    check_eq("kill self signal zero succeeds", sc2(SYS_KILL, pid, 0), 0);
    check_eq("kill missing pid signal zero returns ESRCH", sc2(SYS_KILL, 99999, 0), -ESRCH);
    check_eq("kill rejects negative signal", sc2(SYS_KILL, pid, -1), -1);
    check_eq("kill rejects signal above NSIG", sc2(SYS_KILL, pid, 32), -1);
    check_eq("kill process group signal zero succeeds", sc2(SYS_KILL, -pid, 0), 0);
    check_eq("sched_yield succeeds", sc0(SYS_YIELD), 0);
    req[0] = 0;
    req[1] = 0;
    check_eq("nanosleep zero interval succeeds", sc2(SYS_NANOSLEEP, (long)req, 0), 0);
    check_eq("nanosleep rejects null request", sc2(SYS_NANOSLEEP, 0, 0), -1);
}

static void test_signal_masks(void)
{
    uint32_t newmask = 1u << 10;
    uint32_t oldmask = 0xFFFFFFFFu;
    uint32_t mask2;
    uint32_t act[8];
    uint32_t oldact[8];
    uint32_t rt_new[2];
    uint32_t rt_oldmask[2];
    unsigned char rt_old[8];

    check_eq("sigprocmask sets mask", sc3(SYS_SIGPROCMASK, SIG_SETMASK, (long)&newmask, (long)&oldmask), 0);
    check_eq("sigprocmask reports previous empty mask", oldmask, 0);
    oldmask = 0;
    check_eq("sigprocmask query succeeds", sc3(SYS_SIGPROCMASK, SIG_SETMASK, 0, (long)&oldmask), 0);
    check_eq("sigprocmask query returns blocked signal", oldmask, newmask);
    check_eq("rt_sigprocmask query succeeds", sc4(SYS_RT_SIGPROCMASK, SIG_SETMASK, 0, (long)rt_old, sizeof(rt_old)), 0);
    check_eq("rt_sigprocmask returns low blocked mask", (long)get_u32(rt_old, 0), newmask);
    check_eq("sigprocmask unblocks mask", sc3(SYS_SIGPROCMASK, SIG_UNBLOCK, (long)&newmask, 0), 0);

    for (unsigned i = 0; i < 8; i++) {
        act[i] = 0;
        oldact[i] = 0xFFFFFFFFu;
    }
    act[0] = SIG_IGN;
    check_eq("rt_sigaction installs ignore handler", sc4(SYS_RT_SIGACTION, SIGTERM, (long)act, (long)oldact, 8), 0);
    check_eq("rt_sigaction reports old default handler", oldact[0], SIG_DFL);
    oldact[0] = 0xFFFFFFFFu;
    check_eq("rt_sigaction query succeeds", sc4(SYS_RT_SIGACTION, SIGTERM, 0, (long)oldact, 8), 0);
    check_eq("rt_sigaction query reports ignore handler", oldact[0], SIG_IGN);
    act[0] = SIG_DFL;
    check_eq("rt_sigaction restores default handler", sc4(SYS_RT_SIGACTION, SIGTERM, (long)act, 0, 8), 0);

    oldact[0] = 0xFFFFFFFFu;
    check_eq("sigaction installs ignore handler", sc3(SYS_SIGACTION, SIGTERM, SIG_IGN, (long)&oldact[0]), 0);
    check_eq("sigaction reports old default handler", oldact[0], SIG_DFL);
    check_eq("sigaction restores default handler", sc3(SYS_SIGACTION, SIGTERM, SIG_DFL, (long)&oldact[0]), 0);
    check_eq("sigaction reports old ignore handler", oldact[0], SIG_IGN);
    check_eq("sigaction rejects SIGKILL", sc3(SYS_SIGACTION, SIGKILL, SIG_IGN, 0), -1);
    check_eq("sigaction rejects signal zero", sc3(SYS_SIGACTION, 0, SIG_IGN, 0), -1);

    mask2 = 1u << 12;
    check_eq("sigprocmask blocks additional signal", sc3(SYS_SIGPROCMASK, SIG_BLOCK, (long)&mask2, 0), 0);
    oldmask = 0;
    check_eq("sigprocmask query after block succeeds", sc3(SYS_SIGPROCMASK, SIG_SETMASK, 0, (long)&oldmask), 0);
    check_eq("sigprocmask query after block returns signal", oldmask, mask2);
    mask2 = (1u << SIGKILL) | (1u << SIGSTOP) | (1u << 12);
    check_eq("sigprocmask accepts unblockable mask", sc3(SYS_SIGPROCMASK, SIG_SETMASK, (long)&mask2, 0), 0);
    oldmask = 0;
    check_eq("sigprocmask query after unblockable mask succeeds", sc3(SYS_SIGPROCMASK, SIG_SETMASK, 0, (long)&oldmask), 0);
    check_eq("sigprocmask clears unblockable signals", oldmask, 1u << 12);
    rt_new[0] = 1u << 8;
    rt_new[1] = 0xFFFFFFFFu;
    rt_oldmask[0] = 0xFFFFFFFFu;
    rt_oldmask[1] = 0xFFFFFFFFu;
    check_eq("rt_sigprocmask setmask succeeds", sc4(SYS_RT_SIGPROCMASK, SIG_SETMASK, (long)rt_new, (long)rt_oldmask, sizeof(rt_new)), 0);
    check_eq("rt_sigprocmask old high word is zero", rt_oldmask[1], 0);
    check_eq("rt_sigaction rejects small sigset", sc4(SYS_RT_SIGACTION, SIGTERM, 0, 0, 2), -1);
    oldmask = 0;
    sc3(SYS_SIGPROCMASK, SIG_SETMASK, (long)&oldmask, 0);
}

static void test_filesystem(void)
{
    unsigned char st[144];
    unsigned char stx[256];
    unsigned char sfs[84];
    char buf[32];
    int fd;
    long n;

    check_eq("open missing path returns ENOENT", sc3(SYS_OPEN, (long)"/missing-linuxabi", 0, 0), -ENOENT);
    check_eq("access existing file succeeds", sc2(SYS_ACCESS, (long)"/hello.txt", 0), 0);
    check_eq("access missing path returns ENOENT", sc2(SYS_ACCESS, (long)"/missing-linuxabi", 0), -ENOENT);

    fd = (int)sc3(SYS_OPEN, (long)"/linuxabi.tmp", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check_ok("open creates writable file", fd);
    if (fd >= 0) {
        check_eq("write returns byte count", sc3(SYS_WRITE, fd, (long)"abcdef", 6), 6);
        n = sc3(SYS_FCNTL64, fd, F_GETFL, 0);
        check_eq("F_GETFL preserves O_RDWR access mode", n & O_ACCMODE, O_RDWR);
        check_eq("zero-length write skips null buffer", sc3(SYS_WRITE, fd, 0, 0), 0);
        check_eq("lseek rewinds file", sc3(SYS_LSEEK, fd, 0, SEEK_SET), 0);
        check_eq("zero-length read skips null buffer", sc3(SYS_READ, fd, 0, 0), 0);
        n = sc3(SYS_READ, fd, (long)buf, 6);
        if (n == 6 && memeq(buf, "abcdef", 6))
            pass("read returns written bytes");
        else
            fail("read returns written bytes", n, 6);
        check_eq("lseek end reports file size", sc3(SYS_LSEEK, fd, 0, SEEK_END), 6);
        check_eq("lseek rejects invalid whence", sc3(SYS_LSEEK, fd, 0, 99), -1);
        check_eq("fstat64 succeeds", sc2(SYS_FSTAT64, fd, (long)st), 0);
        check_eq("fstat64 reports file size", (long)get_u64(st, 44), 6);
        check_eq("fstatfs64 succeeds", sc3(SYS_FSTATFS64, fd, sizeof(sfs), (long)sfs), 0);
        check_eq("fstatfs64 reports 4096 block size", (long)get_u32(sfs, 4), 4096);
        check_eq("chmod existing path succeeds", sc2(SYS_CHMOD, (long)"/linuxabi.tmp", 0600), 0);
        check_eq("chown32 existing path succeeds", sc3(SYS_CHOWN32, (long)"/linuxabi.tmp", 0, 0), 0);
        check_eq("lchown existing path succeeds", sc3(SYS_LCHOWN, (long)"/linuxabi.tmp", 0, 0), 0);
        check_eq("readlink regular file returns EINVAL", sc3(SYS_READLINK, (long)"/linuxabi.tmp", (long)buf, sizeof(buf)), -EINVAL);
        check_eq("close created file succeeds", sc1(SYS_CLOSE, fd), 0);
    }

    fd = (int)sc3(SYS_OPEN, (long)"/hello.txt", 0, 0);
    check_ok("open hello read-only succeeds", fd);
    if (fd >= 0) {
        n = sc3(SYS_READ, fd, (long)buf, 5);
        check_eq("read hello prefix byte count", n, 5);
        if (n == 5 && memeq(buf, "Hello", 5))
            pass("read hello prefix contents");
        else
            fail("read hello prefix contents", n, 5);
        check_eq("write read-only file fails", sc3(SYS_WRITE, fd, (long)"x", 1), -1);
        check_eq("close hello read-only succeeds", sc1(SYS_CLOSE, fd), 0);
    }

    check_eq("stat64 sees created file", sc2(SYS_STAT64, (long)"/linuxabi.tmp", (long)st), 0);
    check_eq("stat64 reports created file size", (long)get_u64(st, 44), 6);
    check_eq("lstat64 sees created file", sc2(SYS_LSTAT64, (long)"/linuxabi.tmp", (long)st), 0);
    check_eq("lstat64 reports created file size", (long)get_u64(st, 44), 6);
    check_eq("old stat sees created file", sc2(SYS_STAT, (long)"/linuxabi.tmp", (long)st), 0);
    check_eq("utimensat existing path succeeds", sc4(SYS_UTIMENSAT, AT_FDCWD, (long)"/linuxabi.tmp", 0, 0), 0);
    check_eq("statx existing path succeeds", sc5(SYS_STATX, AT_FDCWD, (long)"/linuxabi.tmp", 0, 0x7FF, (long)stx), 0);
    check_eq("statx reports created file size", (long)get_u64(stx, 40), 6);
    check_eq("statfs64 root succeeds", sc3(SYS_STATFS64, (long)"/", sizeof(sfs), (long)sfs), 0);
    check_eq("open directory write-only returns EISDIR", sc3(SYS_OPEN, (long)"/", O_WRONLY, 0), -EISDIR);
    check_eq("open regular file with O_DIRECTORY returns ENOTDIR", sc3(SYS_OPEN, (long)"/hello.txt", O_DIRECTORY, 0), -ENOTDIR);
    check_eq("open existing file with O_CREAT|O_EXCL returns EEXIST",
             sc3(SYS_OPEN, (long)"/hello.txt", O_CREAT | O_EXCL | O_RDWR, 0644), -EEXIST);
    check_eq("stat64 missing path returns ENOENT", sc2(SYS_STAT64, (long)"/missing-linuxabi", (long)st), -ENOENT);
    check_eq("lstat64 missing path returns ENOENT", sc2(SYS_LSTAT64, (long)"/missing-linuxabi", (long)st), -ENOENT);
    check_eq("statx missing path returns ENOENT", sc5(SYS_STATX, AT_FDCWD, (long)"/missing-linuxabi", 0, 0x7FF, (long)stx), -ENOENT);
    check_eq("chmod missing path returns ENOENT", sc2(SYS_CHMOD, (long)"/missing-linuxabi", 0600), -ENOENT);
    check_eq("chown32 missing path returns ENOENT", sc3(SYS_CHOWN32, (long)"/missing-linuxabi", 0, 0), -ENOENT);
    check_eq("lchown missing path returns ENOENT", sc3(SYS_LCHOWN, (long)"/missing-linuxabi", 0, 0), -ENOENT);
    check_eq("readlink missing path returns ENOENT", sc3(SYS_READLINK, (long)"/missing-linuxabi", (long)buf, sizeof(buf)), -ENOENT);
    check_eq("statfs64 missing path returns ENOENT", sc3(SYS_STATFS64, (long)"/missing-linuxabi", sizeof(sfs), (long)sfs), -ENOENT);
    check_eq("fstatfs64 invalid fd fails", sc3(SYS_FSTATFS64, 99, sizeof(sfs), (long)sfs), -1);
    check_eq("utimensat missing path returns ENOENT", sc4(SYS_UTIMENSAT, AT_FDCWD, (long)"/missing-linuxabi", 0, 0), -ENOENT);
    check_eq("rename succeeds", sc2(SYS_RENAME, (long)"/linuxabi.tmp", (long)"/linuxabi.renamed"), 0);
    check_eq("unlink renamed file succeeds", sc1(SYS_UNLINK, (long)"/linuxabi.renamed"), 0);
    check_eq("unlink missing file fails", sc1(SYS_UNLINK, (long)"/linuxabi.renamed"), -1);
}

static void test_directories(void)
{
    unsigned char dents[512];
    char cwd[64];
    int fd;
    long n;

    check_eq("mkdir creates directory", sc2(SYS_MKDIR, (long)"/linuxabi.dir", 0755), 0);
    check_eq("chdir enters directory", sc1(SYS_CHDIR, (long)"/linuxabi.dir"), 0);
    n = sc2(SYS_GETTIMEOFDAY, (long)cwd, 0);
    check_eq("gettimeofday succeeds before getcwd", n, 0);
    check_eq("mkdir nested relative succeeds", sc2(SYS_MKDIR, (long)"nested", 0755), 0);
    check_eq("chdir nested relative succeeds", sc1(SYS_CHDIR, (long)"nested"), 0);
    n = sc2(SYS_GETCWD, (long)cwd, sizeof(cwd));
    check_ok("getcwd nested returns length", n);
    if (n > 0 && streq(cwd, "/linuxabi.dir/nested"))
        pass("getcwd nested reports path");
    else
        fail("getcwd nested reports path", n, 1);
    n = sc2(SYS_CHDIR, (long)"/", 0);
    check_eq("chdir root after nested succeeds", n, 0);
    check_eq("rmdir nested directory succeeds", sc1(SYS_RMDIR, (long)"/linuxabi.dir/nested"), 0);
    check_eq("chdir returns to root", sc1(SYS_CHDIR, (long)"/"), 0);
    n = sc2(SYS_GETCWD, (long)cwd, sizeof(cwd));
    if (n >= 2 && streq(cwd, "/"))
        pass("getcwd reports root");
    else
        fail("getcwd reports root", n, 2);
    check_eq("getcwd tiny buffer fails", sc2(SYS_GETCWD, (long)cwd, 1), -1);
    check_eq("chdir missing path fails", sc1(SYS_CHDIR, (long)"/missing-dir"), -1);

    fd = (int)sc3(SYS_OPEN, (long)"/", O_DIRECTORY, 0);
    check_ok("open root directory succeeds", fd);
    if (fd >= 0) {
        n = sc3(SYS_GETDENTS64, fd, (long)dents, sizeof(dents));
        if (n > 0 && dirents64_contains(dents, n, "bin"))
            pass("getdents64 lists bin");
        else
            fail("getdents64 lists bin", n, 1);
        check_eq("getdents64 lists dot", n > 0 && dirents64_contains(dents, n, "."), 1);
        check_eq("getdents64 lists dotdot", n > 0 && dirents64_contains(dents, n, ".."), 1);
        check_eq("close directory succeeds", sc1(SYS_CLOSE, fd), 0);
    }
    fd = (int)sc3(SYS_OPEN, (long)"/", O_DIRECTORY, 0);
    check_ok("open root for old getdents succeeds", fd);
    if (fd >= 0) {
        n = sc3(SYS_GETDENTS, fd, (long)dents, sizeof(dents));
        check_ok("getdents reads root entries", n);
        check_eq("getdents lists bin", n > 0 && dirents_contains(dents, n, "bin"), 1);
        check_eq("getdents lists dot", n > 0 && dirents_contains(dents, n, "."), 1);
        check_eq("getdents lists dotdot", n > 0 && dirents_contains(dents, n, ".."), 1);
        check_eq("close old getdents directory succeeds", sc1(SYS_CLOSE, fd), 0);
    }
    check_eq("getdents invalid fd fails", sc3(SYS_GETDENTS, 99, (long)dents, sizeof(dents)), -1);
    check_eq("getdents64 invalid fd fails", sc3(SYS_GETDENTS64, 99, (long)dents, sizeof(dents)), -1);
    check_eq("rmdir removes empty directory", sc1(SYS_RMDIR, (long)"/linuxabi.dir"), 0);
}

static void test_file_variants(void)
{
    unsigned char st[144];
    unsigned char buf[8];
    uint32_t iov[4];
    uint32_t pos[2];
    int fd;
    long n;

    fd = (int)sc2(SYS_CREAT, (long)"/linuxabi.vec", 0644);
    check_ok("creat creates vector file", fd);
    if (fd >= 0) {
        n = sc3(SYS_FCNTL64, fd, F_GETFL, 0);
        check_eq("creat fd reports O_WRONLY access mode", n & O_ACCMODE, O_WRONLY);
        iov[0] = (uint32_t)"ab";
        iov[1] = 2;
        iov[2] = (uint32_t)"cd";
        iov[3] = 2;
        check_eq("writev writes split buffers", sc3(SYS_WRITEV, fd, (long)iov, 2), 4);
        check_eq("writev zero count returns zero", sc3(SYS_WRITEV, fd, (long)iov, 0), 0);
        pos[0] = 0xFFFFFFFFu;
        pos[1] = 0xFFFFFFFFu;
        check_eq("_llseek rewinds vector file", sc5(SYS__LLSEEK, fd, 0, 0, (long)pos, SEEK_SET), 0);
        check_eq("_llseek reports zero offset", pos[0] | pos[1], 0);
        iov[0] = (uint32_t)&buf[0];
        iov[1] = 2;
        iov[2] = (uint32_t)&buf[2];
        iov[3] = 2;
        n = sc3(SYS_READV, fd, (long)iov, 2);
        check_eq("readv reads split buffers", n, 4);
        check_eq("readv preserves first byte", buf[0], 'a');
        check_eq("readv preserves last byte", buf[3], 'd');
        check_eq("lseek current offset after readv", sc3(SYS_LSEEK, fd, 0, SEEK_CUR), 4);
        check_eq("lseek sets vector offset one", sc3(SYS_LSEEK, fd, 1, SEEK_SET), 1);
        n = sc3(SYS_READ, fd, (long)buf, 2);
        check_eq("read after lseek returns byte count", n, 2);
        check_eq("read after lseek preserves byte", buf[0], 'b');
        check_eq("ftruncate64 grows vector file", sc3(SYS_FTRUNCATE64, fd, 6, 0), 0);
        check_eq("fstat64 sees ftruncate grow", sc2(SYS_FSTAT64, fd, (long)st), 0);
        check_eq("fstat64 reports ftruncate grow size", (long)get_u64(st, 44), 6);
        check_eq("ftruncate64 shrinks vector file", sc3(SYS_FTRUNCATE64, fd, 2, 0), 0);
        check_eq("fstat64 sees ftruncate size", sc2(SYS_FSTAT64, fd, (long)st), 0);
        check_eq("fstat64 reports ftruncate size", (long)get_u64(st, 44), 2);
        check_eq("close vector file succeeds", sc1(SYS_CLOSE, fd), 0);
    }

    check_eq("truncate64 missing path returns ENOENT", sc3(SYS_TRUNCATE64, (long)"/missing-linuxabi", 1, 0), -ENOENT);
    check_eq("truncate64 rejects oversize length", sc3(SYS_TRUNCATE64, (long)"/linuxabi.vec", 0, 1), -EINVAL);
    check_eq("ftruncate64 invalid fd fails", sc3(SYS_FTRUNCATE64, 99, 1, 0), -1);
    check_eq("truncate64 grows vector path", sc3(SYS_TRUNCATE64, (long)"/linuxabi.vec", 5, 0), 0);
    check_eq("stat64 sees truncate size", sc2(SYS_STAT64, (long)"/linuxabi.vec", (long)st), 0);
    check_eq("stat64 reports truncate size", (long)get_u64(st, 44), 5);
    check_eq("unlink vector file succeeds", sc1(SYS_UNLINK, (long)"/linuxabi.vec"), 0);

    fd = (int)sc3(SYS_OPEN, (long)"/hello.txt", 0, 0);
    check_ok("open hello for sendfile64 succeeds", fd);
    if (fd >= 0) {
        int out;

        pos[0] = 0;
        pos[1] = 0;
        check_eq("sendfile64 zero count returns zero", sc4(SYS_SENDFILE64, 1, fd, (long)pos, 0), 0);
        check_eq("sendfile64 copies requested bytes", sc4(SYS_SENDFILE64, 1, fd, (long)pos, 5), 5);
        check_eq("sendfile64 advances offset pointer", pos[0], 5);
        check_eq("sendfile64 bad out fd fails", sc4(SYS_SENDFILE64, 99, fd, (long)pos, 1), -1);
        out = (int)sc3(SYS_OPEN, (long)"/linuxabi.sendfile", O_CREAT | O_RDWR | O_TRUNC, 0644);
        check_ok("open sendfile64 regular output succeeds", out);
        if (out >= 0) {
            pos[0] = 0;
            pos[1] = 0;
            check_eq("sendfile64 copies to regular output fd",
                     sc4(SYS_SENDFILE64, out, fd, (long)pos, 5), 5);
            check_eq("sendfile64 regular output advances offset pointer", pos[0], 5);
            check_eq("close sendfile output succeeds", sc1(SYS_CLOSE, out), 0);
        }
        out = (int)sc3(SYS_OPEN, (long)"/linuxabi.sendfile", 0, 0);
        check_ok("open sendfile64 output for read succeeds", out);
        if (out >= 0) {
            n = sc3(SYS_READ, out, (long)buf, 5);
            check_eq("read sendfile64 regular output byte count", n, 5);
            if (n == 5 && memeq(buf, "Hello", 5))
                pass("sendfile64 regular output contents");
            else
                fail("sendfile64 regular output contents", n, 5);
            check_eq("close sendfile readback succeeds", sc1(SYS_CLOSE, out), 0);
        }
        check_eq("unlink sendfile output succeeds", sc1(SYS_UNLINK, (long)"/linuxabi.sendfile"), 0);
        check_eq("close sendfile input succeeds", sc1(SYS_CLOSE, fd), 0);
    }
}

static void test_at_syscalls(void)
{
    unsigned char st[144];
    char buf[16];
    int fd;
    int dirfd;
    int dirfd2;
    long n;

    fd = (int)sc4(SYS_OPENAT, AT_FDCWD, (long)"/hello.txt", 0, 0);
    check_ok("openat AT_FDCWD hello succeeds", fd);
    if (fd >= 0) {
        n = sc3(SYS_READ, fd, (long)buf, 5);
        check_eq("read openat hello byte count", n, 5);
        if (n == 5 && memeq(buf, "Hello", 5))
            pass("read openat hello contents");
        else
            fail("read openat hello contents", n, 5);
        check_eq("close openat hello succeeds", sc1(SYS_CLOSE, fd), 0);
    }

    check_eq("mkdirat AT_FDCWD creates directory", sc3(SYS_MKDIRAT, AT_FDCWD, (long)"/linuxabi.at", 0755), 0);
    dirfd = (int)sc3(SYS_OPEN, (long)"/linuxabi.at", O_DIRECTORY, 0);
    check_ok("open at directory for at syscalls succeeds", dirfd);
    if (dirfd >= 0) {
        check_eq("fstatat64 AT_EMPTY_PATH succeeds", sc4(SYS_FSTATAT64, dirfd, (long)"", (long)st, AT_EMPTY_PATH), 0);
        check_eq("fstatat64 AT_EMPTY_PATH reports directory", (long)(get_u32(st, 16) & S_IFMT), S_IFDIR);
        fd = (int)sc4(SYS_OPENAT, dirfd, (long)"child", O_CREAT | O_RDWR | O_TRUNC, 0644);
        check_ok("openat relative create succeeds", fd);
        if (fd >= 0) {
            check_eq("write openat relative file succeeds", sc3(SYS_WRITE, fd, (long)"xyz", 3), 3);
            check_eq("close openat relative file succeeds", sc1(SYS_CLOSE, fd), 0);
        }
        check_eq("faccessat relative file succeeds", sc3(SYS_FACCESSAT, dirfd, (long)"child", 0), 0);
        check_eq("fstatat64 relative file succeeds", sc4(SYS_FSTATAT64, dirfd, (long)"child", (long)st, 0), 0);
        check_eq("fstatat64 relative file size", (long)get_u64(st, 44), 3);
        check_eq("readlinkat regular file returns EINVAL", sc4(SYS_READLINKAT, dirfd, (long)"child", (long)buf, sizeof(buf)), -EINVAL);
        check_eq("unlinkat relative file succeeds", sc3(SYS_UNLINKAT, dirfd, (long)"child", 0), 0);
        check_eq("faccessat missing relative returns ENOENT", sc3(SYS_FACCESSAT, dirfd, (long)"child", 0), -ENOENT);
        check_eq("mkdirat relative subdir succeeds", sc3(SYS_MKDIRAT, dirfd, (long)"subdir", 0755), 0);
        check_eq("unlinkat AT_REMOVEDIR removes subdir", sc3(SYS_UNLINKAT, dirfd, (long)"subdir", AT_REMOVEDIR), 0);
        check_eq("close at directory succeeds", sc1(SYS_CLOSE, dirfd), 0);
    }

    check_eq("unlinkat AT_FDCWD removes directory", sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/linuxabi.at", AT_REMOVEDIR), 0);
    check_eq("openat missing path returns ENOENT", sc4(SYS_OPENAT, AT_FDCWD, (long)"/missing-at", 0, 0), -ENOENT);
    check_eq("openat invalid dirfd fails", sc4(SYS_OPENAT, 99, (long)"missing", 0, 0), -1);
    check_eq("mkdirat invalid dirfd fails", sc3(SYS_MKDIRAT, 99, (long)"missing", 0755), -1);
    check_eq("fstatat64 rejects invalid flags with EINVAL", sc4(SYS_FSTATAT64, AT_FDCWD, (long)"/hello.txt", (long)st, 0x80000000u), -EINVAL);
    check_eq("faccessat rejects invalid mode", sc3(SYS_FACCESSAT, AT_FDCWD, (long)"/hello.txt", 8), -EINVAL);
    check_eq("unlinkat rejects invalid flags", sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/hello.txt", 0x80000000u), -EINVAL);
    check_eq("readlinkat missing returns ENOENT", sc4(SYS_READLINKAT, AT_FDCWD, (long)"/missing-at", (long)buf, sizeof(buf)), -ENOENT);

    check_eq("mkdirat creates rename source directory", sc3(SYS_MKDIRAT, AT_FDCWD, (long)"/linuxabi.at-src", 0755), 0);
    check_eq("mkdirat creates rename dest directory", sc3(SYS_MKDIRAT, AT_FDCWD, (long)"/linuxabi.at-dst", 0755), 0);
    dirfd = (int)sc3(SYS_OPEN, (long)"/linuxabi.at-src", O_DIRECTORY, 0);
    check_ok("open rename source directory succeeds", dirfd);
    dirfd2 = (int)sc3(SYS_OPEN, (long)"/linuxabi.at-dst", O_DIRECTORY, 0);
    check_ok("open rename dest directory succeeds", dirfd2);
    if (dirfd >= 0 && dirfd2 >= 0) {
        fd = (int)sc4(SYS_OPENAT, dirfd, (long)"oldname", O_CREAT | O_RDWR | O_TRUNC, 0644);
        check_ok("openat creates rename source file", fd);
        if (fd >= 0)
            check_eq("close rename source file succeeds", sc1(SYS_CLOSE, fd), 0);
        check_eq("renameat relative file succeeds", sc4(SYS_RENAMEAT, dirfd, (long)"oldname", dirfd2, (long)"newname"), 0);
        check_eq("renameat removes old relative name", sc3(SYS_FACCESSAT, dirfd, (long)"oldname", 0), -ENOENT);
        check_eq("renameat creates new relative name", sc3(SYS_FACCESSAT, dirfd2, (long)"newname", 0), 0);
        check_eq("fchmodat relative file succeeds", sc3(SYS_FCHMODAT, dirfd2, (long)"newname", 0600), 0);
        check_eq("fchownat relative file succeeds", sc5(SYS_FCHOWNAT, dirfd2, (long)"newname", 0, 0, 0), 0);
        check_eq("futimesat relative file succeeds", sc3(SYS_FUTIMESAT, dirfd2, (long)"newname", 0), 0);
        check_eq("utimensat relative file succeeds", sc4(SYS_UTIMENSAT, dirfd2, (long)"newname", 0, 0), 0);
        check_eq("utimensat accepts symlink nofollow flag", sc4(SYS_UTIMENSAT, dirfd2, (long)"newname", 0, AT_SYMLINK_NOFOLLOW), 0);
        check_eq("utimensat rejects invalid flags", sc4(SYS_UTIMENSAT, dirfd2, (long)"newname", 0, 0x80000000u), -EINVAL);
        check_eq("linkat creates relative hardlink", sc5(SYS_LINKAT, dirfd2, (long)"newname", dirfd2, (long)"hardlink", 0), 0);
        check_eq("symlinkat creates relative symlink", sc3(SYS_SYMLINKAT, (long)"newname", dirfd2, (long)"symlink"), 0);
        n = sc4(SYS_READLINKAT, dirfd2, (long)"symlink", (long)buf, sizeof(buf));
        check_eq("readlinkat symlink byte count", n, 7);
        if (n == 7 && memeq(buf, "newname", 7))
            pass("readlinkat symlink contents");
        else
            fail("readlinkat symlink contents", n, 7);
        check_eq("fstatat64 nofollow symlink succeeds", sc4(SYS_FSTATAT64, dirfd2, (long)"symlink", (long)st, AT_SYMLINK_NOFOLLOW), 0);
        check_eq("fstatat64 nofollow reports symlink", (long)(get_u32(st, 16) & S_IFMT), S_IFLNK);
        fd = (int)sc4(SYS_OPENAT, dirfd2, (long)"symlink", 0, 0);
        check_ok("openat follows relative symlink", fd);
        if (fd >= 0)
            check_eq("close symlink target succeeds", sc1(SYS_CLOSE, fd), 0);
        check_eq("linkat default hardlinks symlink itself", sc5(SYS_LINKAT, dirfd2, (long)"symlink", dirfd2, (long)"symlink-hard", 0), 0);
        n = sc4(SYS_READLINKAT, dirfd2, (long)"symlink-hard", (long)buf, sizeof(buf));
        check_eq("readlinkat hardlinked symlink byte count", n, 7);
        if (n == 7 && memeq(buf, "newname", 7))
            pass("readlinkat hardlinked symlink contents");
        else
            fail("readlinkat hardlinked symlink contents", n, 7);
        check_eq("linkat follow hardlinks symlink target", sc5(SYS_LINKAT, dirfd2, (long)"symlink", dirfd2, (long)"follow-hard", AT_SYMLINK_FOLLOW), 0);
        check_eq("readlinkat followed hardlink returns EINVAL", sc4(SYS_READLINKAT, dirfd2, (long)"follow-hard", (long)buf, sizeof(buf)), -EINVAL);
        check_eq("mkdirat creates symlink target directory", sc3(SYS_MKDIRAT, dirfd2, (long)"realdir", 0755), 0);
        fd = (int)sc4(SYS_OPENAT, dirfd2, (long)"realdir/inside", O_CREAT | O_RDWR | O_TRUNC, 0644);
        check_ok("openat creates file inside target directory", fd);
        if (fd >= 0) {
            check_eq("write file inside target directory succeeds", sc3(SYS_WRITE, fd, (long)"via", 3), 3);
            check_eq("close file inside target directory succeeds", sc1(SYS_CLOSE, fd), 0);
        }
        check_eq("symlinkat creates directory symlink", sc3(SYS_SYMLINKAT, (long)"realdir", dirfd2, (long)"dirlink"), 0);
        fd = (int)sc4(SYS_OPENAT, dirfd2, (long)"dirlink/inside", 0, 0);
        check_ok("openat follows symlink directory prefix", fd);
        if (fd >= 0) {
            n = sc3(SYS_READ, fd, (long)buf, 3);
            check_eq("read through symlink directory prefix byte count", n, 3);
            if (n == 3 && memeq(buf, "via", 3))
                pass("read through symlink directory prefix contents");
            else
                fail("read through symlink directory prefix contents", n, 3);
            check_eq("close symlink directory prefix file succeeds", sc1(SYS_CLOSE, fd), 0);
        }
        check_eq("fstatat64 follows symlink directory prefix", sc4(SYS_FSTATAT64, dirfd2, (long)"dirlink/inside", (long)st, 0), 0);
        check_eq("fstatat64 nofollow reports directory symlink", sc4(SYS_FSTATAT64, dirfd2, (long)"dirlink", (long)st, AT_SYMLINK_NOFOLLOW), 0);
        check_eq("fstatat64 directory symlink mode", (long)(get_u32(st, 16) & S_IFMT), S_IFLNK);
        check_eq("symlinkat creates broken symlink", sc3(SYS_SYMLINKAT, (long)"missing-target", dirfd2, (long)"broken"), 0);
        n = sc4(SYS_READLINKAT, dirfd2, (long)"broken", (long)buf, 4);
        check_eq("readlinkat broken symlink truncates", n, 4);
        if (n == 4 && memeq(buf, "miss", 4))
            pass("readlinkat broken symlink truncated contents");
        else
            fail("readlinkat broken symlink truncated contents", n, 4);
        check_eq("openat broken symlink returns ENOENT", sc4(SYS_OPENAT, dirfd2, (long)"broken", 0, 0), -ENOENT);
        check_eq("symlinkat creates loop a", sc3(SYS_SYMLINKAT, (long)"loop-b", dirfd2, (long)"loop-a"), 0);
        check_eq("symlinkat creates loop b", sc3(SYS_SYMLINKAT, (long)"loop-a", dirfd2, (long)"loop-b"), 0);
        check_eq("openat symlink loop returns ELOOP", sc4(SYS_OPENAT, dirfd2, (long)"loop-a", 0, 0), -ELOOP);
        check_eq("unlinkat removes loop a", sc3(SYS_UNLINKAT, dirfd2, (long)"loop-a", 0), 0);
        check_eq("unlinkat removes loop b", sc3(SYS_UNLINKAT, dirfd2, (long)"loop-b", 0), 0);
        check_eq("unlinkat removes broken symlink", sc3(SYS_UNLINKAT, dirfd2, (long)"broken", 0), 0);
        check_eq("unlinkat removes directory symlink", sc3(SYS_UNLINKAT, dirfd2, (long)"dirlink", 0), 0);
        check_eq("unlinkat removes symlink target file", sc3(SYS_UNLINKAT, dirfd2, (long)"realdir/inside", 0), 0);
        check_eq("unlinkat removes symlink target directory", sc3(SYS_UNLINKAT, dirfd2, (long)"realdir", AT_REMOVEDIR), 0);
        check_eq("unlinkat removes hardlinked symlink", sc3(SYS_UNLINKAT, dirfd2, (long)"symlink-hard", 0), 0);
        check_eq("unlinkat removes symlink", sc3(SYS_UNLINKAT, dirfd2, (long)"symlink", 0), 0);
        check_eq("unlinkat removes renamed relative file", sc3(SYS_UNLINKAT, dirfd2, (long)"newname", 0), 0);
        check_eq("faccessat followed hardlink survives original unlink", sc3(SYS_FACCESSAT, dirfd2, (long)"follow-hard", 0), 0);
        fd = (int)sc4(SYS_OPENAT, dirfd2, (long)"follow-hard", 0, 0);
        check_ok("openat followed hardlink after source unlink succeeds", fd);
        if (fd >= 0)
            check_eq("close followed hardlink succeeds", sc1(SYS_CLOSE, fd), 0);
        check_eq("unlinkat removes followed hardlink", sc3(SYS_UNLINKAT, dirfd2, (long)"follow-hard", 0), 0);
        check_eq("faccessat hardlink survives original unlink", sc3(SYS_FACCESSAT, dirfd2, (long)"hardlink", 0), 0);
        fd = (int)sc4(SYS_OPENAT, dirfd2, (long)"hardlink", 0, 0);
        check_ok("openat hardlink after source unlink succeeds", fd);
        if (fd >= 0)
            check_eq("close hardlink succeeds", sc1(SYS_CLOSE, fd), 0);
        check_eq("unlinkat removes hardlink", sc3(SYS_UNLINKAT, dirfd2, (long)"hardlink", 0), 0);
        check_eq("close rename source directory succeeds", sc1(SYS_CLOSE, dirfd), 0);
        check_eq("close rename dest directory succeeds", sc1(SYS_CLOSE, dirfd2), 0);
    }
    check_eq("unlinkat removes rename source directory", sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/linuxabi.at-src", AT_REMOVEDIR), 0);
    check_eq("unlinkat removes rename dest directory", sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/linuxabi.at-dst", AT_REMOVEDIR), 0);
    check_eq("renameat invalid old dirfd fails", sc4(SYS_RENAMEAT, 99, (long)"old", AT_FDCWD, (long)"/missing-at"), -1);
    check_eq("fchmodat missing returns ENOENT", sc3(SYS_FCHMODAT, AT_FDCWD, (long)"/missing-at", 0600), -ENOENT);
    check_eq("fchownat missing returns ENOENT", sc5(SYS_FCHOWNAT, AT_FDCWD, (long)"/missing-at", 0, 0, 0), -ENOENT);
    check_eq("futimesat missing returns ENOENT", sc3(SYS_FUTIMESAT, AT_FDCWD, (long)"/missing-at", 0), -ENOENT);
}

static void test_fds_and_pipes(void)
{
    int fds[2];
    unsigned char pfd[8];
    unsigned char winsz[8];
    unsigned char termios[60];
    uint32_t available = 0xFFFFFFFFu;
    char c = 0;
    int dupfd;
    long n;

    check_eq("ioctl TIOCGWINSZ succeeds", sc3(SYS_IOCTL, 1, TIOCGWINSZ, (long)winsz), 0);
    check_eq("ioctl TCGETS succeeds", sc3(SYS_IOCTL, 0, TCGETS, (long)termios), 0);
    check_eq("ioctl TCSETS succeeds", sc3(SYS_IOCTL, 0, TCSETS, (long)termios), 0);
    check_eq("zero-length tty read returns immediately", sc3(SYS_READ, 0, 0, 0), 0);
    check_eq("pipe creates read and write fds", sc1(SYS_PIPE, (long)fds), 0);
    if (fds[0] >= 0 && fds[1] >= 0) {
        check_eq("pipe write returns byte count", sc3(SYS_WRITE, fds[1], (long)"z", 1), 1);
        n = sc3(SYS_FCNTL64, fds[0], F_GETFL, 0);
        check_eq("pipe read fd reports O_RDONLY access mode", n & O_ACCMODE, 0);
        n = sc3(SYS_FCNTL64, fds[1], F_GETFL, 0);
        check_eq("pipe write fd reports O_WRONLY access mode", n & O_ACCMODE, O_WRONLY);
        check_eq("ioctl FIONREAD succeeds", sc3(SYS_IOCTL, fds[0], FIONREAD, (long)&available), 0);
        check_eq("ioctl FIONREAD reports byte count", available, 1);
        n = 1u << (unsigned)fds[0];
        check_eq("select reports readable pipe", sc5(SYS__NEWSELECT, fds[0] + 1, (long)&n, 0, 0, 0), 1);
        put_u32(pfd, 0, (uint32_t)fds[0]);
        put_u16(pfd, 4, POLLIN);
        put_u16(pfd, 6, 0);
        check_eq("poll reports readable pipe", sc3(SYS_POLL, (long)pfd, 1, 0), 1);
        put_u32(pfd, 0, (uint32_t)fds[1]);
        put_u16(pfd, 4, POLLOUT);
        put_u16(pfd, 6, 0);
        check_eq("poll reports writable pipe", sc3(SYS_POLL, (long)pfd, 1, 0), 1);
        check_eq("poll writable pipe sets POLLOUT", get_u32(pfd, 4) >> 16, POLLOUT);
        check_eq("pipe read returns byte count", sc3(SYS_READ, fds[0], (long)&c, 1), 1);
        if (c == 'z')
            pass("pipe preserves byte value");
        else
            fail("pipe preserves byte value", c, 'z');
        available = 0xFFFFFFFFu;
        check_eq("ioctl FIONREAD after read succeeds", sc3(SYS_IOCTL, fds[0], FIONREAD, (long)&available), 0);
        check_eq("ioctl FIONREAD after read reports empty", available, 0);
        check_eq("zero-length pipe read returns immediately", sc3(SYS_READ, fds[0], 0, 0), 0);
        check_eq("zero-length pipe write returns immediately", sc3(SYS_WRITE, fds[1], 0, 0), 0);
        dupfd = (int)sc1(SYS_DUP, fds[1]);
        check_ok("dup duplicates fd", dupfd);
        if (dupfd >= 0) {
            check_eq("close dup fd succeeds", sc1(SYS_CLOSE, dupfd), 0);
        }
        dupfd = (int)sc2(SYS_DUP2, fds[1], 9);
        check_eq("dup2 duplicates to requested fd", dupfd, 9);
        if (dupfd == 9)
            check_eq("close dup2 fd succeeds", sc1(SYS_CLOSE, dupfd), 0);
        dupfd = (int)sc2(SYS_DUP2, fds[1], fds[1]);
        check_eq("dup2 same fd returns same fd", dupfd, fds[1]);
        dupfd = (int)sc3(SYS_FCNTL64, fds[1], F_DUPFD, 10);
        check_ok("fcntl F_DUPFD duplicates fd", dupfd);
        if (dupfd >= 0)
            check_eq("close fcntl duplicate succeeds", sc1(SYS_CLOSE, dupfd), 0);
        check_eq("fcntl F_SETFD accepts close-on-exec flag", sc3(SYS_FCNTL64, fds[1], F_SETFD, FD_CLOEXEC), 0);
        check_eq("fcntl F_GETFD reports close-on-exec", sc3(SYS_FCNTL64, fds[1], F_GETFD, 0), FD_CLOEXEC);
        check_eq("fcntl F_SETFD clears close-on-exec flag", sc3(SYS_FCNTL64, fds[1], F_SETFD, 0), 0);
        check_eq("fcntl F_GETFD reports cleared flags", sc3(SYS_FCNTL64, fds[1], F_GETFD, 0), 0);
        dupfd = (int)sc3(SYS_FCNTL64, fds[1], F_DUPFD_CLOEXEC, 10);
        check_ok("fcntl F_DUPFD_CLOEXEC duplicates fd", dupfd);
        if (dupfd >= 0) {
            check_eq("fcntl F_DUPFD_CLOEXEC sets close-on-exec",
                     sc3(SYS_FCNTL64, dupfd, F_GETFD, 0), FD_CLOEXEC);
            check_eq("close fcntl cloexec duplicate succeeds", sc1(SYS_CLOSE, dupfd), 0);
        }
        n = sc3(SYS_FCNTL64, fds[1], F_GETFL, 0);
        check_ok("fcntl F_GETFL succeeds", n);
        check_eq("fcntl F_SETFL accepts append flag", sc3(SYS_FCNTL64, fds[1], F_SETFL, O_APPEND), 0);
        put_u32(pfd, 0, 8);
        put_u16(pfd, 4, POLLIN);
        put_u16(pfd, 6, 0);
        check_eq("poll invalid fd reports ready", sc3(SYS_POLL, (long)pfd, 1, 0), 1);
        check_eq("poll invalid fd sets POLLNVAL", get_u32(pfd, 4) >> 16, POLLNVAL);
        check_eq("poll zero nfds returns zero", sc3(SYS_POLL, (long)pfd, 0, 0), 0);
        check_eq("poll null fds fails", sc3(SYS_POLL, 0, 1, 0), -1);
        check_eq("pipe null pointer fails", sc1(SYS_PIPE, 0), -1);
        check_eq("dup invalid fd fails", sc1(SYS_DUP, 99), -1);
        check_eq("dup2 invalid old fd fails", sc2(SYS_DUP2, 99, 8), -1);
        check_eq("close pipe read fd succeeds", sc1(SYS_CLOSE, fds[0]), 0);
        check_eq("close pipe write fd succeeds", sc1(SYS_CLOSE, fds[1]), 0);
    }
    check_eq("pipe2 cloexec creates read and write fds", sc2(SYS_PIPE2, (long)fds, O_CLOEXEC), 0);
    if (fds[0] >= 0 && fds[1] >= 0) {
        check_eq("pipe2 O_CLOEXEC sets read fd flag", sc3(SYS_FCNTL64, fds[0], F_GETFD, 0), FD_CLOEXEC);
        check_eq("pipe2 O_CLOEXEC sets write fd flag", sc3(SYS_FCNTL64, fds[1], F_GETFD, 0), FD_CLOEXEC);
        check_eq("pipe2 write returns byte count", sc3(SYS_WRITE, fds[1], (long)"q", 1), 1);
        check_eq("pipe2 read returns byte count", sc3(SYS_READ, fds[0], (long)&c, 1), 1);
        if (c == 'q')
            pass("pipe2 preserves byte value");
        else
            fail("pipe2 preserves byte value", c, 'q');
        check_eq("close pipe2 read fd succeeds", sc1(SYS_CLOSE, fds[0]), 0);
        check_eq("close pipe2 write fd succeeds", sc1(SYS_CLOSE, fds[1]), 0);
    }
    check_eq("pipe2 nonblock creates fds", sc2(SYS_PIPE2, (long)fds, O_NONBLOCK), 0);
    if (fds[0] >= 0 && fds[1] >= 0) {
        check_eq("nonblocking empty pipe read returns EAGAIN",
                 sc3(SYS_READ, fds[0], (long)&c, 1), -EAGAIN);
        n = sc3(SYS_FCNTL64, fds[0], F_GETFL, 0);
        check_eq("pipe2 O_NONBLOCK appears in read flags", n & O_NONBLOCK, O_NONBLOCK);
        n = sc3(SYS_FCNTL64, fds[1], F_GETFL, 0);
        check_eq("pipe2 O_NONBLOCK appears in write flags", n & O_NONBLOCK, O_NONBLOCK);
        check_eq("close nonblock pipe read fd succeeds", sc1(SYS_CLOSE, fds[0]), 0);
        check_eq("close nonblock pipe write fd succeeds", sc1(SYS_CLOSE, fds[1]), 0);
    }
    check_eq("pipe2 rejects unsupported flags", sc2(SYS_PIPE2, (long)fds, 0x80000000u), -EINVAL);
}

static void test_memory_and_time(void)
{
    uint32_t mmap_args[6];
    uint32_t ts[2];
    uint32_t ts64[4];
    uint32_t tv[2];
    uint32_t tz[2];
    uint32_t rlim[4];
    long addr;
    long brk0;
    long n;

    brk0 = sc1(SYS_BRK, 0);
    check_ok("brk query succeeds", brk0);
    check_eq("brk second query is stable", sc1(SYS_BRK, 0), brk0);
    check_eq("brk refuses below heap", sc1(SYS_BRK, brk0 - 1), brk0);
    check_eq("brk grow succeeds", sc1(SYS_BRK, brk0 + 4096), brk0 + 4096);
    check_eq("brk query reports grow", sc1(SYS_BRK, 0), brk0 + 4096);
    check_eq("brk shrink succeeds", sc1(SYS_BRK, brk0), brk0);
    mmap_args[0] = 0;
    mmap_args[1] = 4096;
    mmap_args[2] = PROT_READ | PROT_WRITE;
    mmap_args[3] = MAP_PRIVATE | MAP_ANONYMOUS;
    mmap_args[4] = 0xFFFFFFFFu;
    mmap_args[5] = 0;
    addr = sc1(SYS_MMAP, (long)mmap_args);
    if (!is_linux_error(addr))
        pass("mmap anonymous page succeeds");
    else
        fail("mmap anonymous page succeeds", addr, 0);
    if (!is_linux_error(addr)) {
        char *p = (char *)(uintptr_t)(uint32_t)addr;
        p[0] = 'm';
        p[4095] = 'z';
        check_eq("mprotect anonymous page succeeds", sc3(SYS_MPROTECT, addr, 4096, PROT_READ | PROT_WRITE), 0);
        check_eq("munmap anonymous page succeeds", sc2(SYS_MUNMAP, addr, 4096), 0);
    }
    n = sc3(SYS_OPEN, (long)"/hello.txt", 0, 0);
    check_ok("open hello for mmap succeeds", n);
    if (n >= 0) {
        mmap_args[0] = 0;
        mmap_args[1] = 4096;
        mmap_args[2] = PROT_READ;
        mmap_args[3] = MAP_PRIVATE;
        mmap_args[4] = (uint32_t)n;
        mmap_args[5] = 0;
        addr = sc1(SYS_MMAP, (long)mmap_args);
        if (!is_linux_error(addr))
            pass("mmap private file page succeeds");
        else
            fail("mmap private file page succeeds", addr, 0);
        if (!is_linux_error(addr)) {
            char *p = (char *)(uintptr_t)(uint32_t)addr;
            if (memeq(p, "Hello", 5))
                pass("mmap private file contents");
            else
                fail("mmap private file contents", p[0], 'H');
            check_eq("munmap file page succeeds", sc2(SYS_MUNMAP, addr, 4096), 0);
        }
        check_eq("close mmap input succeeds", sc1(SYS_CLOSE, n), 0);
    }
    mmap_args[0] = 0;
    mmap_args[1] = 4096;
    mmap_args[2] = PROT_READ | PROT_WRITE;
    mmap_args[3] = MAP_PRIVATE | MAP_ANONYMOUS;
    mmap_args[4] = 0xFFFFFFFFu;
    mmap_args[5] = 0;
    mmap_args[1] = 0;
    check_eq("mmap rejects zero length", sc1(SYS_MMAP, (long)mmap_args), -1);
    mmap_args[1] = 4096;
    mmap_args[2] = 8;
    check_eq("mmap rejects invalid protection", sc1(SYS_MMAP, (long)mmap_args), -1);
    mmap_args[2] = PROT_READ | PROT_WRITE;
    check_eq("clock_gettime realtime succeeds", sc2(SYS_CLOCK_GETTIME, 0, (long)ts), 0);
    check_eq("clock_gettime monotonic succeeds", sc2(SYS_CLOCK_GETTIME, 1, (long)ts), 0);
    check_eq("clock_gettime rejects invalid clock", sc2(SYS_CLOCK_GETTIME, 99, (long)ts), -1);
    check_eq("clock_gettime64 realtime succeeds", sc2(SYS_CLOCK_GETTIME64, 0, (long)ts64), 0);
    check_eq("clock_gettime64 high seconds zero", ts64[1], 0);
    check_eq("clock_gettime64 rejects invalid clock", sc2(SYS_CLOCK_GETTIME64, 99, (long)ts64), -1);
    check_eq("gettimeofday succeeds", sc2(SYS_GETTIMEOFDAY, (long)tv, 0), 0);
    tz[0] = 1;
    tz[1] = 1;
    n = sc2(SYS_GETTIMEOFDAY, 0, (long)tz);
    if (n == 0 && tz[0] == 0 && tz[1] == 0)
        pass("gettimeofday zeroes timezone");
    else
        fail("gettimeofday zeroes timezone", n, 0);
    for (unsigned i = 0; i < 4; i++)
        rlim[i] = 0xA5A5A5A5u;
    n = sc4(SYS_PRLIMIT64, 0, RLIMIT_STACK, 0, (long)rlim);
    if (n == 0 && (rlim[0] != 0xA5A5A5A5u || rlim[1] != 0xA5A5A5A5u) &&
        (rlim[2] != 0xA5A5A5A5u || rlim[3] != 0xA5A5A5A5u))
        pass("prlimit64 reads stack limit");
    else
        fail("prlimit64 reads stack limit", n, 0);
    check_eq("mmap2 rejects zero length", sc6(SYS_MMAP2, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), -1);
    n = sc3(SYS_OPEN, (long)"/hello.txt", 0, 0);
    check_ok("open hello for mmap2 succeeds", n);
    if (n >= 0) {
        addr = sc6(SYS_MMAP2, 0, 4096, PROT_READ, MAP_PRIVATE, n, 0);
        if (!is_linux_error(addr))
            pass("mmap2 private file page succeeds");
        else
            fail("mmap2 private file page succeeds", addr, 0);
        if (!is_linux_error(addr)) {
            char *p = (char *)(uintptr_t)(uint32_t)addr;
            if (memeq(p, "Hello", 5))
                pass("mmap2 private file contents");
            else
                fail("mmap2 private file contents", p[0], 'H');
            check_eq("munmap mmap2 file page succeeds", sc2(SYS_MUNMAP, addr, 4096), 0);
        }
        check_eq("close mmap2 input succeeds", sc1(SYS_CLOSE, n), 0);
    }
    addr = sc6(SYS_MMAP2, 0, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!is_linux_error(addr))
        pass("mmap2 anonymous page succeeds");
    else
        fail("mmap2 anonymous page succeeds", addr, 0);
    if (!is_linux_error(addr))
        check_eq("munmap mmap2 page succeeds", sc2(SYS_MUNMAP, addr, 4096), 0);
}

static void test_process_wait(void)
{
    long pid = sc0(SYS_FORK);

    if (pid == 0) {
        sc1(SYS_EXIT, 7);
        for (;;)
            ;
    }
    check_ok("fork returns child pid in parent", pid);
    if (pid > 0) {
        int status = 0;
        long waited = sc3(SYS_WAITPID, pid, (long)&status, 0);
        check_eq("waitpid returns child pid", waited, pid);
        check_eq("waitpid reports encoded exit status", status, 7 << 8);
    }

    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT, 9);
        for (;;)
            ;
    }
    check_ok("second fork returns child pid in parent", pid);
    if (pid > 0) {
        int status = 0;
        uint32_t rusage[18];
        long waited;

        for (unsigned i = 0; i < 18; i++)
            rusage[i] = 0xFFFFFFFFu;
        waited = sc4(SYS_WAIT4, pid, (long)&status, 0, (long)rusage);
        check_eq("wait4 returns child pid", waited, pid);
        check_eq("wait4 reports encoded exit status", status, 9 << 8);
        check_eq("wait4 clears rusage", rusage[0], 0);
    }

    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT, 11);
        for (;;)
            ;
    }
    check_ok("third fork returns child pid in parent", pid);
    if (pid > 0) {
        int status = 0;
        long waited = sc3(SYS_WAITPID, 0, (long)&status, 0);
        check_eq("waitpid zero selector returns same-pgrp child", waited, pid);
        check_eq("waitpid zero selector reports status", status, 11 << 8);
    }

    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT, 12);
        for (;;)
            ;
    }
    check_ok("fourth fork returns child pid in parent", pid);
    if (pid > 0) {
        int status = 0;
        long pgid = sc1(SYS_GETPGID, 0);
        long waited = sc4(SYS_WAIT4, -pgid, (long)&status, 0, 0);
        check_eq("wait4 negative pgid selector returns child", waited, pid);
        check_eq("wait4 negative pgid selector reports status", status, 12 << 8);
    }
}

static void write_summary(void)
{
    emit_raw("LINUXABI SUMMARY passed ");
    emit_uint((unsigned)passed);
    emit_raw("/");
    emit_uint((unsigned)total);
    emit_raw("\n");
    emit_raw("LINUXABI DONE\n");
}

int main(void)
{
    log_fd = (int)sc3(SYS_OPEN, (long)"/dufs/linuxabi.log",
                      O_CREAT | O_WRONLY | O_TRUNC, 0644);
    emit_raw("LINUXABI BEGIN\n");
    test_identity();
    test_system_info();
    test_process_controls();
    test_signal_masks();
    test_filesystem();
    test_directories();
    test_file_variants();
    test_at_syscalls();
    test_fds_and_pipes();
    test_memory_and_time();
    test_process_wait();
    write_summary();
    if (log_fd >= 0)
        sc1(SYS_CLOSE, log_fd);
    sc1(SYS_EXIT_GROUP, passed == total ? 0 : 1);
    return passed == total ? 0 : 1;
}

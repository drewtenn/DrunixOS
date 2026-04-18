/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PROCESS_H
#define PROCESS_H

#include "wait.h"
#include "vma.h"
#include "vfs.h"
#include <stdint.h>

/*
 * Virtual address layout for user processes.
 *
 * Code and data are loaded at whatever address the ELF specifies
 * (typically 0x400000 for a user-land executable).
 *
 * Heap: begins at the page-rounded end of the BSS segment and grows upward
 *   via SYS_BRK.  May not grow into the user stack region.
 *
 * Stack: 4 pages (16 KB) mapped just below the 3 GB boundary.
 *   Highest virtual address: USER_STACK_TOP (exclusive, stack grows down).
 *   Lowest mapped page:      USER_STACK_TOP - USER_STACK_PAGES * 4096.
 */
#define USER_STACK_TOP    0xC0000000u
#define USER_STACK_PAGES  4
#define USER_STACK_MAX_PAGES  64u

/* The heap ceiling: the heap must not grow at or above this address. */
#define USER_HEAP_MAX  (USER_STACK_TOP - (uint32_t)(USER_STACK_MAX_PAGES) * 0x1000u)

/*
 * Anonymous mmap() allocations are placed high in the address space and grow
 * downward so they never collide with the ELF image / brk-managed heap.
 */
#define USER_MMAP_MIN  0x40000000u

/* Per-process kernel stack size (heap-allocated, one per process).
 * Must be large enough to hold the deepest kernel call chain: the
 * DUFS block I/O path (syscall → vfs → fs_list/dir_lookup → block_map)
 * stacks two 4096-byte block buffers simultaneously, so 8 KB is not
 * enough.  16 KB gives comfortable headroom. */
#define KSTACK_SIZE       16384u

/* ── Signal constants ───────────────────────────────────────────────────── */

/* Number of signals supported.  Bitmasks are uint32_t so max is 32. */
#define NSIG     32

/* Signal dispositions stored in sig_handlers[]. */
#define SIG_DFL  0u   /* take default action */
#define SIG_IGN  1u   /* ignore the signal   */

/* Signal numbers (Linux-compatible). */
#define SIGINT   2    /* interactive attention (Ctrl-C) — default: terminate */
#define SIGILL   4    /* illegal instruction                               */
#define SIGTRAP  5    /* breakpoint / trace trap                           */
#define SIGABRT  6    /* abort                                             */
#define SIGFPE   8    /* arithmetic exception                              */
#define SIGKILL  9    /* uncatchable kill                — default: terminate */
#define SIGSEGV  11   /* invalid memory reference                          */
#define SIGPIPE  13   /* write to broken pipe            — default: terminate */
#define SIGTERM  15   /* polite termination request      — default: terminate */
#define SIGCHLD  17   /* child exited                    — default: ignore    */
#define SIGCONT  18   /* continue a stopped process      — default: continue  */
#define SIGSTOP  19   /* uncatchable stop                — default: stop      */
#define SIGTSTP  20   /* terminal stop (Ctrl-Z)          — default: stop      */

/*
 * Per-process open-file table.  Every fd — including 0 (stdin), 1 (stdout),
 * and 2 (stderr) — is stored as a typed entry in this array.  The type field
 * determines how SYS_READ / SYS_WRITE dispatch the I/O.
 */
#define MAX_FDS  16u

/*
 * fd_type_t — what kind of resource an fd refers to.
 */
typedef enum {
    FD_TYPE_NONE       = 0,  /* slot is free                          */
    FD_TYPE_FILE       = 1,  /* VFS-backed regular file               */
    FD_TYPE_CHARDEV    = 2,  /* character device (e.g. keyboard)      */
    FD_TYPE_STDOUT     = 3,  /* VGA console output via print_bytes()  */
    FD_TYPE_PIPE_READ  = 4,  /* read end of a kernel pipe             */
    FD_TYPE_PIPE_WRITE = 5,  /* write end of a kernel pipe            */
    FD_TYPE_TTY        = 6,  /* TTY line discipline (stdin)           */
    FD_TYPE_PROCFILE   = 7,  /* synthetic procfs file                 */
    FD_TYPE_DIR        = 8,  /* directory fd for Linux getdents64     */
    FD_TYPE_BLOCKDEV   = 9,  /* read-only block device fd             */
} fd_type_t;

/*
 * file_handle_t — a single entry in a process's open-file table.
 *
 * The union carries type-specific data; only the member matching `type`
 * is valid.
 */
typedef struct {
    uint32_t  type;      /* fd_type_t — uint32_t for alignment         */
    uint32_t  writable;  /* 1 if the fd is open for writing            */
    uint32_t  append;    /* 1 if writes append at end of file          */
    union {
        struct {
            vfs_file_ref_t ref; /* owning mount and backend inode      */
            uint32_t inode_num; /* inode number for Linux stat output  */
            uint32_t size;      /* cached file size in bytes           */
            uint32_t offset;    /* current read/write offset           */
        } file;
        struct {
            char name[12];      /* chardev name (e.g. "stdin")         */
        } chardev;
        struct {
            char name[VFS_DEV_NAME_MAX]; /* blkdev name (e.g. "sda1") */
            uint32_t offset;    /* current byte offset within device     */
            uint32_t size;      /* cached device size in bytes           */
        } blockdev;
        struct {
            uint32_t pipe_idx;  /* index into global pipe_table[]      */
        } pipe;
        struct {
            uint32_t tty_idx;   /* index into tty_table[]              */
        } tty;
        struct {
            uint32_t kind;      /* procfs_file_kind_t value            */
            uint32_t pid;       /* owning /proc/<pid> entry, or 0      */
            uint32_t index;     /* auxiliary selector (e.g. fd number) */
            uint32_t size;      /* last observed synthetic file size   */
            uint32_t offset;    /* current read offset                 */
        } proc;
        struct {
            char path[128];     /* VFS path without leading slash       */
            uint32_t index;     /* next directory-entry index           */
        } dir;
    } u;
} file_handle_t;

typedef enum {
    PROC_UNUSED   = 0,  /* slot is free */
    PROC_READY    = 1,  /* runnable, not currently on CPU */
    PROC_RUNNING  = 2,  /* currently executing on CPU */
    PROC_ZOMBIE   = 3,  /* exited, slot pending reclaim */
    PROC_BLOCKED  = 4,  /* asleep on a wait queue or timed wait */
    PROC_STOPPED  = 5,  /* stopped by SIGSTOP/SIGTSTP, waiting for SIGCONT */
} proc_state_t;

/*
 * trap_frame_t — saved CPU state for an exception / interrupt entry.
 *
 * Field order matches the stack layout built by isr.asm and used by the
 * scheduler's signal-delivery path.
 */
typedef struct __attribute__((packed)) {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_saved, ebx, edx, ecx, eax;
    uint32_t vector, error_code;
    uint32_t eip, cs, eflags;
    uint32_t user_esp, user_ss;   /* valid only when (cs & 3) == 3 */
} trap_frame_t;

/*
 * crash_info_t — synchronous fault context captured for a user process.
 *
 * The scheduler uses this to emit a core file if the resulting signal takes
 * the default fatal action.
 */
typedef struct {
    uint32_t     valid;     /* 1 if a fault context is present */
    uint32_t     signum;    /* signal generated for the fault */
    uint32_t     cr2;       /* faulting linear address for #PF, else 0 */
    trap_frame_t frame;     /* saved register frame at fault time */
} crash_info_t;

typedef struct process {
    uint32_t     pd_phys;       /* physical address of the process page directory */
    uint32_t     entry;         /* ELF entry point virtual address */
    uint32_t     user_stack;    /* user stack top (initial ESP, grows down) */
    uint32_t     kstack_top;    /* top of the per-process kernel stack (TSS.ESP0) */
    uint32_t     kstack_bottom; /* base of the heap-allocated kernel stack block */
    uint32_t     saved_esp;     /* kernel ESP saved at last preemption; 0 = never run */
    uint32_t     pid;           /* process ID (1-based, assigned by scheduler) */
    uint32_t     state;         /* proc_state_t — uint32_t to keep struct size aligned */
    wait_queue_t *wait_queue;   /* queue this process is blocked on, or NULL       */
    process_t    *wait_next;    /* intrusive link while queued                      */
    uint32_t     wait_deadline; /* scheduler tick deadline for timed waits         */
    uint32_t     wait_deadline_set; /* 1 when wait_deadline is active             */
    uint32_t     heap_start;    /* first byte of the heap region (page-rounded BSS end) */
    uint32_t     brk;           /* current program break (first unmapped heap byte) */
    uint32_t     image_start;   /* lowest mapped PT_LOAD address       */
    uint32_t     image_end;     /* page-rounded end of highest PT_LOAD */
    uint32_t     stack_low_limit; /* lowest currently mapped user stack page */
    uint32_t     pgid;          /* process group ID (0 filled in by sched_add → pid) */
    uint32_t     sid;           /* session ID (0 filled in by sched_add → pid)       */
    uint32_t     tty_id;        /* controlling TTY index attached to this process     */
    uint32_t     parent_pid;    /* PID of the process that created this one  */
    uint32_t     exit_status;   /* exit code from SYS_EXIT, read by waiter   */
    uint32_t     umask;         /* Linux umask(2), inherited across fork/exec */
    uint32_t     user_tls_base;  /* Linux i386 set_thread_area descriptor base */
    uint32_t     user_tls_limit; /* Linux i386 set_thread_area descriptor limit */
    uint32_t     user_tls_limit_in_pages; /* nonzero when descriptor uses 4 KiB pages */
    uint32_t     user_tls_present; /* nonzero when the per-process TLS slot is valid */
    wait_queue_t state_waiters; /* waitpid waiters for exit/stop transitions */
    vm_area_t    vmas[PROCESS_MAX_VMAS]; /* sorted by ascending start address */
    uint32_t     vma_count;
    /*
     * Executable basename (up to 15 chars + NUL), copied from argv[0] at
     * process_create time.  Written into the NT_PRPSINFO note of core dumps
     * so that GDB can identify the binary without being told explicitly.
     */
    char         name[16];
    /*
     * First 79 bytes of the process command line, assembled from argv[].
     * Written into NT_PRPSINFO.pr_psargs so debuggers can display the
     * command that generated a core file.
     */
    char         psargs[80];
    /*
     * FXSAVE image: 512 bytes holding x87, MMX, and XMM0-XMM7 state.
     * Saved/restored on every context switch via fxsave/fxrstor.  Keep the
     * member itself explicitly 16-byte aligned so adding process metadata
     * fields above it does not silently break the required alignment.
     */
    uint8_t       fpu_state[512] __attribute__((aligned(16)));
    /*
     * Unified open-file table.  All fds 0–MAX_FDS-1 live here, including
     * stdin (0), stdout (1), and stderr (2).  Slots are dispatched by
     * open_files[fd].type.  Comes after fpu_state to keep the 16-byte
     * alignment of fpu_state intact.
     */
    file_handle_t open_files[MAX_FDS];
    /*
     * Current working directory, stored as a root-relative DUFS path with no
     * leading slash.  Empty string means the process is at the filesystem root.
     * Inherited across fork(); set via SYS_CHDIR; read via SYS_GETCWD.
     */
    char          cwd[4096];

    /*
     * Signal handling state.
     *
     * sig_pending: bitmask of signals that have been sent but not yet
     *   delivered.  Bit N corresponds to signal number N.
     *
     * sig_blocked: bitmask of signals that are currently masked.  A pending
     *   signal whose bit is set here is not delivered until unblocked.
     *   SIGKILL (bit 9) cannot be blocked — the kernel always clears it.
     *
     * sig_handlers: per-signal disposition.  SIG_DFL (0) means take the
     *   default action (terminate for most signals); SIG_IGN (1) means
     *   silently discard; any other value is the virtual address of a
     *   user-space handler function of type void (*)(int).
     *
     * Inherited across fork() — child copies handlers, but starts with an
     * empty sig_pending (POSIX: pending signals are not inherited).
     */
    uint32_t      sig_pending;
    uint32_t      sig_blocked;
    uint32_t      sig_handlers[NSIG];
    crash_info_t  crash;
} __attribute__((aligned(16))) process_t;

/*
 * Upper bounds on the argv passed to process_create.  These keep the kernel
 * copy path simple (a fixed-size scratch buffer in SYS_EXECVE) and guarantee
 * the assembled stack frame fits inside the top user stack page.
 */
#define PROCESS_ARGV_MAX_COUNT  32u
#define PROCESS_ARGV_MAX_BYTES  1024u
#define PROCESS_ENV_MAX_COUNT   32u
#define PROCESS_ENV_MAX_BYTES   1024u

/*
 * process_create: load an ELF binary from disk via the DUFS inode, allocate
 * its address space, and fill in the process descriptor.
 *
 * proc:        output descriptor to be filled in.
 * inode_num:   DUFS inode number of the ELF binary.
 * argv:        kernel-resident NULL-terminated argv array (strings must also be
 *              kernel-resident).  May be NULL if argc == 0.
 * argc:        number of valid entries in argv (not counting the terminator).
 * envp:        kernel-resident NULL-terminated environment array.  May be NULL
 *              if envc == 0.  Entries must be "NAME=VALUE" strings.
 * envc:        number of valid entries in envp (not counting the terminator).
 * inherit_fds: if non-NULL, the new process inherits this fd table (pipe
 *              refcounts are bumped).  Pass NULL to give the process the
 *              default stdin (keyboard) / stdout / stderr (VGA) setup.
 *
 * Lays out argc/argv/envp/auxv/strings at the top of the new process's user
 * stack in Linux/System V i386 ABI order, with ESP pointing at argc on entry.
 *
 * Returns 0 on success, negative on error.
 */
int process_create_file(process_t *proc, vfs_file_ref_t file_ref,
                        const char *const *argv, int argc,
                        const char *const *envp, int envc,
                        const file_handle_t *inherit_fds);

#ifdef KTEST_ENABLED
int process_build_user_stack_frame_for_test(uint32_t pd_phys,
                                            const char *const *argv,
                                            int argc,
                                            const char *const *envp,
                                            int envc,
                                            uint32_t *out_esp);
#endif

/*
 * process_build_initial_frame: synthesise the first kernel-stack frame for a
 * never-run process so a context switch can "return" into process_initial_launch
 * and iret into user mode at proc->entry / proc->user_stack.
 */
void process_build_initial_frame(process_t *proc);

/*
 * process_build_exec_frame: like process_build_initial_frame, but arranges for
 * process_exec_cleanup(old_pd_phys, old_kstack_bottom) to run on the new kernel
 * stack before iret so the replaced image's resources are released safely.
 */
void process_build_exec_frame(process_t *proc, uint32_t old_pd_phys,
                              uint32_t old_kstack_bottom);

/*
 * process_restore_user_tls: load this process's Linux i386 TLS descriptor
 * into the single hardware GDT TLS slot. The scheduler calls this whenever it
 * changes the current process because the GDT itself is global.
 */
void process_restore_user_tls(const process_t *proc);

/*
 * process_launch: switch to the process's address space and perform an iret
 * to ring 3. Does NOT return — the process will eventually call sys_exit.
 */
void process_launch(process_t *proc);

/*
 * process_fork: create a child process that is an exact copy of the parent.
 *
 * Clones the parent's entire user address space (eager page copy, no CoW),
 * allocates a new kernel stack, and replicates the parent's current syscall
 * frame on the child's kernel stack with EAX zeroed so the child's fork()
 * returns 0.  The parent's fork() return value (child PID) is set by the
 * SYS_FORK case in syscall.c.
 *
 * child_out: output descriptor to be filled in (then passed to sched_add).
 * parent:    pointer to the currently running process (from sched_current()).
 *
 * Returns 0 on success, -1 on failure (OOM).
 * Known limitations: no cleanup on partial paging failure; kstack leaked if
 * sched_add subsequently fails.
 */
int process_fork(process_t *child_out, process_t *parent);

/*
 * process_close_all_fds: close every open fd in the process table entry.
 *
 * Flushes writable files, decrements pipe endpoint refcounts, and frees pipe
 * buffers whose last endpoint was closed. Safe to call exactly once during a
 * process's teardown path before the slot becomes a zombie.
 */
void process_close_all_fds(process_t *proc);

/*
 * process_release_user_space: free a process's user-owned paging structures.
 *
 * Walks every present PDE with PG_USER set, frees each present user frame in
 * the page table, then frees the page table itself. Kernel-only PDEs are
 * shared identity mappings and are left untouched. Finally frees the page
 * directory page itself and clears pd_phys.
 */
void process_release_user_space(process_t *proc);

/*
 * process_release_kstack: free a process's heap-allocated kernel stack.
 *
 * Restores the guard page to present before freeing the raw kmalloc region,
 * then clears the recorded stack pointers.
 */
void process_release_kstack(process_t *proc);

/*
 * process_exec_cleanup: helper called from process_asm.asm after an in-place
 * exec has switched to its new kernel stack. Releases the replaced image's old
 * user address space and old kernel stack.
 */
void process_exec_cleanup(uint32_t old_pd_phys, uint32_t old_kstack_bottom);

#endif

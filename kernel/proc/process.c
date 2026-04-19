/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * process.c — process creation, teardown, and per-process resource management.
 */

#include "process.h"
#include "resources.h"
#include "sched.h"
#include "pipe.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "gdt.h"
#include "sse.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "fs.h"
#include <stdint.h>

/* Defined in process_asm.asm */
extern void process_enter_usermode(uint32_t entry, uint32_t user_esp,
                                   uint32_t user_cs, uint32_t user_ds);
extern void process_initial_launch(void);
extern void process_exec_launch(void);

#define LINUX_AT_NULL   0u
#define LINUX_AT_PAGESZ 6u

/*
 * Build the Linux/System V i386 initial stack at the top of a freshly mapped
 * user stack and return the initial ESP the process should start with.
 *
 * The top user-stack page was just mapped by paging_map_page() in the
 * caller, so its PDE/PTE are present in the new page directory.  Because
 * all page-table structures live in the kernel's identity-mapped range
 * (0–128 MB), we can walk them as plain kernel pointers and resolve the
 * physical frame backing USER_STACK_TOP-0x1000.  We then write the frame
 * directly through that physical address (kernel RW, identity-mapped), so
 * no CR3 switch is required.
 *
 * Frame layout (high → low addresses, fits entirely within the top 4 KB
 * user stack page):
 *
 *   +--------------------------+  USER_STACK_TOP
 *   | argv/env strings         |  null-terminated C strings, concatenated
 *   +--------------------------+
 *   | 0..3 bytes zero pad      |  align next word to 4 bytes
 *   +--------------------------+
 *   | auxv terminator value    |
 *   | AT_NULL                  |
 *   | PAGE_SIZE                |
 *   | AT_PAGESZ                |
 *   | NULL                     |  envp terminator
 *   | envp[envc-1] (char*)     |
 *   |  ...                     |
 *   | envp[0]      (char*)     |
 *   | NULL                     |  argv terminator
 *   | argv[argc-1] (char*)     |
 *   |  ...                     |
 *   | argv[0]      (char*)     |  ← argv == ESP + 4
 *   | argc                     |  int argc ← ESP
 *   +--------------------------+
 *
 * Returns 0 on success, negative on error.
 */
static int build_user_stack_frame(uint32_t pd_phys,
                                  const char *const *argv, int argc,
                                  const char *const *envp, int envc,
                                  uint32_t *out_esp)
{
    /* Normalise the "no argv" case so the rest of this function can assume
     * argc is a small non-negative integer. */
    if (argc < 0 || argv == 0)
        argc = 0;
    if (envc < 0 || envp == 0)
        envc = 0;
    if ((uint32_t)argc > PROCESS_ARGV_MAX_COUNT)
        return -1;
    if ((uint32_t)envc > PROCESS_ENV_MAX_COUNT)
        return -1;

    /* Sum the string bytes (including NULs) and bail if they overflow. */
    uint32_t argv_strbytes = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        if (!s) return -1;
        uint32_t n = 0;
        while (s[n]) n++;
        argv_strbytes += n + 1;              /* + NUL terminator */
        if (argv_strbytes > PROCESS_ARGV_MAX_BYTES)
            return -1;
    }
    uint32_t env_strbytes = 0;
    for (int i = 0; i < envc; i++) {
        const char *s = envp[i];
        if (!s) return -1;
        uint32_t n = 0;
        while (s[n]) n++;
        env_strbytes += n + 1;
        if (env_strbytes > PROCESS_ENV_MAX_BYTES)
            return -1;
    }
    uint32_t strbytes = argv_strbytes + env_strbytes;

    /*
     * Compute layout.  Everything is measured as an offset below
     * USER_STACK_TOP.  Grow the frame downward in three chunks:
     *   1. raw string bytes (top of stack)
     *   2. pad to 4-byte alignment
     *   3. Linux raw initial stack words:
     *      argc, argv[], NULL, envp[], NULL, auxv pairs, AT_NULL
     */
    uint32_t strings_off = strbytes;                    /* bytes consumed by strings */
    uint32_t pad = (4u - (strings_off & 3u)) & 3u;
    uint32_t aux_words = 4u;                            /* AT_PAGESZ pair + AT_NULL */
    uint32_t stack_words = 1u + (uint32_t)argc + 1u +
                           (uint32_t)envc + 1u + aux_words;
    uint32_t frame_off = strings_off + pad + stack_words * 4u;

    /* Reject anything that would spill past the top stack page. */
    if (frame_off > 0x1000u)
        return -1;

    /*
     * Resolve the physical page backing the top user stack page (the one
     * holding USER_STACK_TOP-1).  The page directory and page tables live
     * in the identity-mapped kernel region, so we can walk them as plain
     * pointers to their physical addresses.
     */
    uint32_t top_vpage = USER_STACK_TOP - 0x1000u;
    uint32_t pdi       = top_vpage >> 22;
    uint32_t pti       = (top_vpage >> 12) & 0x3FFu;
    uint32_t *pd       = (uint32_t *)pd_phys;
    uint32_t pt_phys   = paging_entry_addr(pd[pdi]);
    if (!pt_phys) return -1;
    uint32_t *pt       = (uint32_t *)pt_phys;
    uint32_t pg_phys   = paging_entry_addr(pt[pti]);
    if (!pg_phys) return -1;

    uint8_t *page_end = (uint8_t *)pg_phys + 0x1000u;   /* one past last byte */

    /*
     * Step 1: copy each string into the top of the stack page and record
     * the user-space virtual address of each copy in a local table.  We
     * write from low-address-first within the reserved string area.
     */
    uint8_t *strbase_k = page_end - strings_off;                /* kernel alias */
    uint32_t strbase_u = USER_STACK_TOP - strings_off;          /* user vaddr of same byte */

    uint32_t uargv_ptrs[PROCESS_ARGV_MAX_COUNT];                /* user vaddrs for argv[] */
    uint32_t uenv_ptrs[PROCESS_ENV_MAX_COUNT];                  /* user vaddrs for envp[] */
    uint32_t write_cursor = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        uint32_t j = 0;
        while (s[j]) {
            strbase_k[write_cursor + j] = (uint8_t)s[j];
            j++;
        }
        strbase_k[write_cursor + j] = 0;
        uargv_ptrs[i] = strbase_u + write_cursor;
        write_cursor += j + 1;
    }
    for (int i = 0; i < envc; i++) {
        const char *s = envp[i];
        uint32_t j = 0;
        while (s[j]) {
            strbase_k[write_cursor + j] = (uint8_t)s[j];
            j++;
        }
        strbase_k[write_cursor + j] = 0;
        uenv_ptrs[i] = strbase_u + write_cursor;
        write_cursor += j + 1;
    }

    /*
     * Step 2: write the Linux raw initial stack just below the strings.  The
     * argv and envp vectors are inline after argc; auxv follows envp.
     */
    uint32_t *tail_k = (uint32_t *)(page_end - frame_off);
    uint32_t idx = 0;

    tail_k[idx++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++)
        tail_k[idx++] = uargv_ptrs[i];
    tail_k[idx++] = 0;                  /* argv terminator */

    for (int i = 0; i < envc; i++)
        tail_k[idx++] = uenv_ptrs[i];
    tail_k[idx++] = 0;                  /* envp terminator */

    tail_k[idx++] = LINUX_AT_PAGESZ;
    tail_k[idx++] = PAGE_SIZE;
    tail_k[idx++] = LINUX_AT_NULL;
    tail_k[idx++] = 0;

    *out_esp = USER_STACK_TOP - frame_off;
    return 0;
}

#ifdef KTEST_ENABLED
int process_build_user_stack_frame_for_test(uint32_t pd_phys,
                                            const char *const *argv,
                                            int argc,
                                            const char *const *envp,
                                            int envc,
                                            uint32_t *out_esp)
{
    return build_user_stack_frame(pd_phys, argv, argc, envp, envc, out_esp);
}
#endif

static uint32_t *build_launch_isr_frame(uint32_t *ksp, const process_t *proc)
{
    /* CPU iret frame — ring-3 target */
    *--ksp = GDT_USER_DS;       /* SS_user  */
    *--ksp = proc->user_stack;  /* ESP_user */
    *--ksp = 0x202;             /* EFLAGS: IF=1, reserved bit 1=1 */
    *--ksp = GDT_USER_CS;       /* CS_user  */
    *--ksp = proc->entry;       /* EIP      */

    /* IRQ stub words */
    *--ksp = 0;                 /* error_code */
    *--ksp = 0;                 /* vector     */

    /* pusha frame (high → low: EAX first, EDI last) */
    *--ksp = 0;  /* EAX */
    *--ksp = 0;  /* ECX */
    *--ksp = 0;  /* EDX */
    *--ksp = 0;  /* EBX */
    *--ksp = 0;  /* ESP_saved (ignored by popa) */
    *--ksp = 0;  /* EBP */
    *--ksp = 0;  /* ESI */
    *--ksp = 0;  /* EDI */

    /* Segment registers (high → low: DS first, GS last) */
    *--ksp = GDT_USER_DS;  /* DS */
    *--ksp = GDT_USER_DS;  /* ES */
    *--ksp = GDT_USER_DS;  /* FS */
    *--ksp = GDT_USER_DS;  /* GS */

    return ksp;
}

static void process_fork_rollback_child(process_t *child_out)
{
    if (!child_out)
        return;

    proc_resource_put_all(child_out);
    process_release_user_space(child_out);
    process_release_kstack(child_out);
    k_memset(child_out, 0, sizeof(*child_out));
}

void process_clone_rollback(process_t *child)
{
    if (!child)
        return;

    proc_resource_put_all(child);
    process_release_kstack(child);
    k_memset(child, 0, sizeof(*child));
}

static int process_clone_duplicate_as(process_t *child_out,
                                      const process_t *parent)
{
    uint32_t new_pd = paging_clone_user_space(parent->pd_phys);
    if (!new_pd) {
        klog_uint("CLONE", "pmm free pages", pmm_free_page_count());
        return -1;
    }

    child_out->pd_phys = new_pd;
    child_out->as = (proc_address_space_t *)kmalloc(sizeof(*child_out->as));
    if (!child_out->as) {
        process_release_user_space(child_out);
        return -1;
    }

    k_memcpy(child_out->as, parent->as, sizeof(*child_out->as));
    child_out->as->refs = 1;
    child_out->as->pd_phys = new_pd;
    return 0;
}

static int process_clone_resources(process_t *child_out,
                                   const process_t *parent,
                                   uint32_t flags)
{
    if (!child_out || !parent || !parent->as || !parent->files ||
        !parent->fs_state || !parent->sig_actions)
        return -1;

    child_out->as = 0;
    child_out->files = 0;
    child_out->fs_state = 0;
    child_out->sig_actions = 0;

    if (flags & CLONE_VM) {
        child_out->as = parent->as;
        child_out->pd_phys = parent->pd_phys;
        child_out->as->refs++;
    } else if (process_clone_duplicate_as(child_out, parent) != 0) {
        return -1;
    }

    if (flags & CLONE_FILES) {
        child_out->files = parent->files;
        child_out->files->refs++;
    } else if (proc_fd_table_dup(&child_out->files, parent->files) != 0) {
        process_clone_rollback(child_out);
        return -1;
    }

    if (flags & CLONE_FS) {
        child_out->fs_state = parent->fs_state;
        child_out->fs_state->refs++;
    } else if (proc_fs_state_dup(&child_out->fs_state,
                                 parent->fs_state) != 0) {
        process_clone_rollback(child_out);
        return -1;
    }

    if (flags & CLONE_SIGHAND) {
        child_out->sig_actions = parent->sig_actions;
        child_out->sig_actions->refs++;
    } else if (proc_sig_actions_dup(&child_out->sig_actions,
                                    parent->sig_actions) != 0) {
        process_clone_rollback(child_out);
        return -1;
    }

    return 0;
}

static int process_clone_kernel_stack(process_t *child_out,
                                      const process_t *parent,
                                      uint32_t child_stack)
{
    uint8_t *kstack_raw =
        (uint8_t *)kmalloc(0x1000u + KSTACK_SIZE + 0xFFFu);
    if (!kstack_raw) {
        klog_uint("CLONE", "heap free bytes", kheap_free_bytes());
        return -1;
    }

    uint8_t *kguard = (uint8_t *)(((uint32_t)kstack_raw + 0xFFFu) & ~0xFFFu);
    paging_guard_page((uint32_t)kguard);
    child_out->kstack_bottom = (uint32_t)kstack_raw;
    child_out->kstack_top = (uint32_t)(kguard + 0x1000u + KSTACK_SIZE);

    if (!parent->kstack_top) {
        if (child_stack)
            child_out->user_stack = child_stack;
        process_build_initial_frame(child_out);
        return 0;
    }

    uint32_t parent_frame = parent->kstack_top - 76u;
    uint32_t child_frame = child_out->kstack_top - 76u;

    k_memcpy((void *)child_frame, (void *)parent_frame, 76);
    ((uint32_t *)child_frame)[11] = 0;
    if (child_stack)
        ((uint32_t *)child_frame)[17] = child_stack;

    uint32_t *ksp = (uint32_t *)child_frame;
    *--ksp = (uint32_t)process_initial_launch;
    *--ksp = 0;
    *--ksp = 0;
    *--ksp = 0;
    *--ksp = 0;

    child_out->saved_esp = (uint32_t)ksp;
    return 0;
}

void process_build_initial_frame(process_t *proc)
{
    if (!proc)
        return;

    uint32_t *ksp = build_launch_isr_frame((uint32_t *)proc->kstack_top, proc);

    /* switch_context pops EDI, ESI, EBX, EBP then ret. */
    *--ksp = (uint32_t)process_initial_launch;  /* return address */
    *--ksp = 0;  /* EBP */
    *--ksp = 0;  /* EBX */
    *--ksp = 0;  /* ESI */
    *--ksp = 0;  /* EDI */

    proc->saved_esp = (uint32_t)ksp;
}

void process_build_exec_frame(process_t *proc, uint32_t old_pd_phys,
                              uint32_t old_kstack_bottom)
{
    if (!proc)
        return;

    uint32_t *ksp = build_launch_isr_frame((uint32_t *)proc->kstack_top, proc);

    /* process_exec_launch consumes these two words before the ISR restore. */
    *--ksp = old_kstack_bottom;
    *--ksp = old_pd_phys;
    *--ksp = (uint32_t)process_exec_launch;  /* return address */
    *--ksp = 0;  /* EBP */
    *--ksp = 0;  /* EBX */
    *--ksp = 0;  /* ESI */
    *--ksp = 0;  /* EDI */

    proc->saved_esp = (uint32_t)ksp;
}

void process_restore_user_tls(const process_t *proc)
{
    if (proc && proc->user_tls_present) {
        gdt_set_user_tls(proc->user_tls_base, proc->user_tls_limit,
                         (int)proc->user_tls_limit_in_pages);
    } else {
        gdt_clear_user_tls();
    }
}

int process_create_file(process_t *proc, vfs_file_ref_t file_ref,
                        const char *const *argv, int argc,
                        const char *const *envp, int envc,
                        const file_handle_t *inherit_fds)
{
    process_t *parent = sched_current();

    k_memset(proc, 0, sizeof(*proc));

    /* Step 1: fresh page directory with kernel mappings copied (no PG_USER) */
    uint32_t pd_phys = paging_create_user_space();
    if (!pd_phys) return -1;

    /* Step 2: parse and load ELF segments into the new address space */
    uint32_t entry       = 0;
    uint32_t image_start = 0;
    uint32_t heap_start  = 0;
    if (elf_load_file(file_ref, pd_phys, &entry, &image_start, &heap_start) != 0)
        return -2;

    /* Step 3: allocate and map the user stack */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t phys = pmm_alloc_page();
        if (!phys) return -3;

        /* Map pages downward from USER_STACK_TOP */
        uint32_t vaddr = USER_STACK_TOP - (uint32_t)(i + 1) * 0x1000;
        if (paging_map_page(pd_phys, vaddr, phys,
                            PG_PRESENT | PG_WRITABLE | PG_USER) != 0)
            return -4;
    }

    /* Step 4: assemble the argc/argv frame at the top of the user stack.
     * Must run after the stack pages are mapped (step 3) and before the
     * process descriptor records the initial ESP. */
    uint32_t initial_esp = USER_STACK_TOP;
    if (build_user_stack_frame(pd_phys, argv, argc, envp, envc, &initial_esp) != 0)
        return -6;

    /* Step 5: allocate a per-process kernel stack from the heap.
     * One extra page is prepended as a non-present guard page: any kernel
     * stack overflow faults immediately rather than silently corrupting the
     * heap.
     *
     * kmalloc does not guarantee page alignment, so we over-allocate by one
     * page and round the raw pointer up to the next 4 KB boundary to find the
     * guard page.  This ensures the guard and kstack are each exactly one
     * contiguous page-aligned region, and that we never accidentally guard a
     * page that contains live heap metadata for an adjacent allocation.
     *
     * Layout (within the allocation):
     *   [ 0 .. (kguard-raw-1) ]   unused alignment padding (0–4095 B)
     *   [ kguard .. kguard+0xFFF ] guard page   (4 KB, non-present)
     *   [ kguard+0x1000 .. top-1 ] kstack        (KSTACK_SIZE bytes)
     *
     * kstack_bottom records the raw kmalloc base (for kfree).
     * kstack_top is the first byte above the usable stack region. */
    uint8_t *kstack_raw = (uint8_t *)kmalloc(0x1000u + KSTACK_SIZE + 0xFFFu);
    if (!kstack_raw) {
        klog_uint("PROC", "process_create kstack heap free", kheap_free_bytes());
        return -5;
    }
    uint8_t *kguard = (uint8_t *)(((uint32_t)kstack_raw + 0xFFFu) & ~0xFFFu);
    paging_guard_page((uint32_t)kguard);

    proc->pd_phys       = pd_phys;
    proc->entry         = entry;
    proc->image_start   = image_start;
    proc->image_end     = heap_start;
    proc->heap_start    = heap_start;
    proc->brk           = heap_start;   /* empty heap: brk == heap_start, no pages committed */
    proc->user_stack    = initial_esp;
    proc->stack_low_limit =
        USER_STACK_TOP - (uint32_t)USER_STACK_PAGES * 0x1000u;
    proc->kstack_bottom = (uint32_t)kstack_raw;
    proc->kstack_top    = (uint32_t)(kguard + 0x1000u + KSTACK_SIZE);
    proc->saved_esp     = 0;       /* scheduler builds the initial switch frame */
    proc->pid           = 0;       /* assigned by sched_add() */
    proc->state         = PROC_UNUSED;  /* sched_add() sets to PROC_READY */
    proc->wait_queue    = 0;
    proc->wait_next     = 0;
    proc->wait_deadline = 0;
    proc->wait_deadline_set = 0;
    proc->pgid          = parent ? parent->pgid : 0;
    proc->sid           = parent ? parent->sid  : 0;
    proc->tty_id        = parent ? parent->tty_id : 0;
    proc->parent_pid    = parent ? parent->pid : 0;
    proc->exit_status   = 0;
    proc->umask         = parent ? parent->umask : 022u;
    proc->user_tls_base  = 0;
    proc->user_tls_limit = 0;
    proc->user_tls_limit_in_pages = 0;
    proc->user_tls_present = 0;
    sched_wait_queue_init(&proc->state_waiters);
    vma_init(proc);
    if (proc->image_start < proc->image_end &&
        vma_add(proc, proc->image_start, proc->image_end,
                VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC |
                VMA_FLAG_PRIVATE,
                VMA_KIND_IMAGE) != 0)
        return -7;
    if (vma_add(proc, proc->heap_start, proc->brk,
                VMA_FLAG_READ | VMA_FLAG_WRITE |
                VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                VMA_KIND_HEAP) != 0)
        return -8;
    if (vma_add(proc,
                USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * PAGE_SIZE,
                USER_STACK_TOP,
                VMA_FLAG_READ | VMA_FLAG_WRITE |
                VMA_FLAG_ANON | VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
                VMA_KIND_STACK) != 0)
        return -9;

    /* Give the process a clean FPU/SSE state (MXCSR=0x1F80, all regs 0) */
    k_memcpy(proc->fpu_state, sse_initial_fpu_state, 512);

    /* Executable basename: find the last '/' and copy the tail, truncated to
     * 15 chars.  Used by the core dump NT_PRPSINFO note. */
    k_memset(proc->name, 0, sizeof(proc->name));
    if (argc > 0 && argv && argv[0]) {
        const char *slash = k_strrchr(argv[0], '/');
        const char *base  = slash ? slash + 1 : argv[0];
        k_strncpy(proc->name, base, 15);
    }

    /* Best-effort command line for NT_PRPSINFO.pr_psargs. */
    k_memset(proc->psargs, 0, sizeof(proc->psargs));
    if (argc > 0 && argv) {
        uint32_t out = 0;
        for (int i = 0; i < argc && out < sizeof(proc->psargs) - 1; i++) {
            const char *s = argv[i];
            if (!s)
                continue;
            if (i > 0)
                proc->psargs[out++] = ' ';
            while (*s && out < sizeof(proc->psargs) - 1)
                proc->psargs[out++] = *s++;
        }
        proc->psargs[out] = '\0';
    }

    /* Start every process at the filesystem root. */
    proc->cwd[0] = '\0';

    /* Signal state: no pending signals, nothing blocked, all dispositions SIG_DFL. */
    proc->sig_pending = 0;
    proc->sig_blocked = 0;
    for (int i = 0; i < NSIG; i++)
        proc->sig_handlers[i] = SIG_DFL;
    proc->crash.valid  = 0;
    proc->crash.signum = 0;
    proc->crash.cr2    = 0;

    if (inherit_fds) {
        /* Inherit the calling process's fd table, bumping pipe refcounts so
         * fd_close_one() in either process does not prematurely free a shared
         * pipe buffer.  This is the Linux exec(2) behaviour: open file
         * descriptors survive across exec unless marked O_CLOEXEC. */
        for (unsigned i = 0; i < MAX_FDS; i++) {
            proc->open_files[i] = inherit_fds[i];
            if (inherit_fds[i].type == FD_TYPE_PIPE_READ) {
                pipe_buf_t *pb = pipe_get((int)inherit_fds[i].u.pipe.pipe_idx);
                if (pb) pb->read_open++;
            } else if (inherit_fds[i].type == FD_TYPE_PIPE_WRITE) {
                pipe_buf_t *pb = pipe_get((int)inherit_fds[i].u.pipe.pipe_idx);
                if (pb) pb->write_open++;
            }
        }
    } else {
        /* Fresh defaults: stdin/stdout/stderr point at the controlling TTY,
         * matching a normal Linux login shell before redirection. */
        for (unsigned i = 0; i < MAX_FDS; i++) {
            proc->open_files[i].type     = FD_TYPE_NONE;
            proc->open_files[i].writable = 0;
            proc->open_files[i].access_mode = 0;
            proc->open_files[i].append   = 0;
        }

        /* fd 0 — stdin: TTY line discipline on the inherited controlling TTY. */
        proc->open_files[0].type          = FD_TYPE_TTY;
        proc->open_files[0].writable      = 1;
        proc->open_files[0].access_mode   = 2;
        proc->open_files[0].u.tty.tty_idx = proc->tty_id;

        /* fd 1 — stdout: writable controlling TTY. */
        proc->open_files[1].type          = FD_TYPE_TTY;
        proc->open_files[1].writable      = 1;
        proc->open_files[1].access_mode   = 2;
        proc->open_files[1].u.tty.tty_idx = proc->tty_id;

        /* fd 2 — stderr: writable controlling TTY. */
        proc->open_files[2].type          = FD_TYPE_TTY;
        proc->open_files[2].writable      = 1;
        proc->open_files[2].access_mode   = 2;
        proc->open_files[2].u.tty.tty_idx = proc->tty_id;
    }

    if (proc_resource_init_fresh(proc) != 0) {
        process_release_user_space(proc);
        process_release_kstack(proc);
        return -10;
    }
    proc_resource_mirror_from_process(proc);

    return 0;
}

void process_launch(process_t *proc)
{
    /* Tell the CPU where to find the kernel stack when INT fires from ring 3 */
    gdt_set_tss_esp0(proc->kstack_top);
    process_restore_user_tls(proc);

    /* Activate the process's own page directory */
    paging_switch_directory(proc->pd_phys);

    /* Restore the process's FPU/SSE state before entering ring 3 */
    __asm__ volatile ("fxrstor %0" :: "m"(*proc->fpu_state));

    /*
     * Transfer control to ring 3.
     * GDT_USER_CS (0x1B) and GDT_USER_DS (0x23) are DPL=3 selectors.
     * process_enter_usermode never returns.
     */
    process_enter_usermode(proc->entry, proc->user_stack,
                           GDT_USER_CS, GDT_USER_DS);
}

int process_fork(process_t *child_out, process_t *parent)
{
    /*
     * Flush the live FPU/SSE registers into parent->fpu_state now.
     * schedule() saves FPU state on a context switch, so if the parent has
     * not been preempted since it last ran, fpu_state[] still holds the
     * snapshot from the previous switch and does not reflect the current
     * register contents.  The child must inherit the true, up-to-date state.
     */
    __asm__ volatile ("fxsave %0" : "=m"(*parent->fpu_state));

    /* Clone the parent's user address space with copy-on-write mappings. */
    uint32_t new_pd = paging_clone_user_space(parent->pd_phys);
    if (!new_pd) {
        klog_uint("FORK", "pmm free pages", pmm_free_page_count());
        return -1;
    }

    /* Shallow-copy all fields; override child-specific ones below. */
    *child_out = *parent;
    child_out->pd_phys    = new_pd;
    child_out->pid        = 0;           /* assigned by sched_add() */
    child_out->state      = PROC_UNUSED; /* sched_add() promotes to PROC_READY */
    child_out->wait_queue = 0;
    child_out->wait_next = 0;
    child_out->wait_deadline = 0;
    child_out->wait_deadline_set = 0;
    child_out->parent_pid = parent->pid;
    child_out->exit_status = 0;
    child_out->tid = 0;
    child_out->tgid = 0;
    child_out->group = 0;
    child_out->clear_child_tid = 0;
    sched_wait_queue_init(&child_out->state_waiters);

    /*
     * Signal state: the child inherits the parent's signal handlers (so
     * installed handlers remain in effect after fork), but starts with an
     * empty pending set — POSIX specifies that pending signals are not
     * inherited across fork().
     */
    child_out->sig_pending = 0;
    child_out->crash.valid  = 0;
    child_out->crash.signum = 0;
    child_out->crash.cr2    = 0;
    if (parent->as || parent->files || parent->fs_state ||
        parent->sig_actions) {
        if (proc_resource_clone_for_fork(child_out, parent) != 0) {
            process_fork_rollback_child(child_out);
            return -1;
        }
    } else {
        child_out->as = 0;
        child_out->files = 0;
        child_out->fs_state = 0;
        child_out->sig_actions = 0;
    }

    /* Allocate an independent kernel stack for the child, with guard page.
     * Same page-aligned layout as process_create: over-allocate by one page
     * and round the raw pointer up to a 4 KB boundary for the guard. */
    uint8_t *kstack_raw = (uint8_t *)kmalloc(0x1000u + KSTACK_SIZE + 0xFFFu);
    if (!kstack_raw) {
        klog_uint("FORK", "heap free bytes", kheap_free_bytes());
        process_fork_rollback_child(child_out);
        return -1;
    }
    uint8_t *kguard = (uint8_t *)(((uint32_t)kstack_raw + 0xFFFu) & ~0xFFFu);
    paging_guard_page((uint32_t)kguard);
    child_out->kstack_bottom = (uint32_t)kstack_raw;
    child_out->kstack_top    = (uint32_t)(kguard + 0x1000u + KSTACK_SIZE);

    /*
     * Replicate the 76-byte ISR frame from the parent's kernel stack onto
     * the child's kernel stack.  The ISR frame is always at kstack_top - 76
     * because INT 0x80 pushes from TSS.ESP0 and the trampoline always pushes
     * the same set of registers. Below that frame we build the usual fake
     * switch_context frame so the first context switch irets into user mode.
     */
    uint32_t parent_frame = parent->kstack_top - 76;
    uint32_t child_frame  = child_out->kstack_top - 76;

    k_memcpy((void *)child_frame, (void *)parent_frame, 76);

    /* Zero the EAX slot (byte offset 44 from frame base = index 11) so the
     * child's fork() returns 0. */
    ((uint32_t *)child_frame)[11] = 0;

    /* Build a fake switch_context frame below the ISR frame.
     * switch_context pops: EDI, ESI, EBX, EBP then ret. */
    uint32_t *ksp = (uint32_t *)child_frame;
    *--ksp = (uint32_t)process_initial_launch;  /* return address */
    *--ksp = 0;  /* EBP */
    *--ksp = 0;  /* EBX */
    *--ksp = 0;  /* ESI */
    *--ksp = 0;  /* EDI */

    child_out->saved_esp = (uint32_t)ksp;

    return 0;
}

int process_clone(process_t *child_out, process_t *parent,
                  uint32_t flags, uint32_t child_stack,
                  uint32_t parent_tidptr, uint32_t tls,
                  uint32_t child_tidptr)
{
    (void)parent_tidptr;

    if (!child_out || !parent)
        return -1;

    __asm__ volatile ("fxsave %0" : "=m"(*parent->fpu_state));

    *child_out = *parent;
    child_out->tid = 0;
    child_out->pid = 0;
    child_out->tgid = (flags & CLONE_THREAD) ? parent->tgid : 0;
    child_out->group = (flags & CLONE_THREAD) ? parent->group : 0;
    child_out->parent_pid =
        (flags & CLONE_THREAD) ? parent->parent_pid : parent->tgid;
    child_out->state = PROC_UNUSED;
    child_out->wait_queue = 0;
    child_out->wait_next = 0;
    child_out->wait_deadline = 0;
    child_out->wait_deadline_set = 0;
    child_out->exit_status = 0;
    child_out->sig_pending = 0;
    child_out->sig_blocked = parent->sig_blocked;
    child_out->clear_child_tid =
        (flags & CLONE_CHILD_CLEARTID) ? child_tidptr : 0;
    child_out->crash.valid = 0;
    child_out->crash.signum = 0;
    child_out->crash.cr2 = 0;
    child_out->kstack_bottom = 0;
    child_out->kstack_top = 0;
    child_out->saved_esp = 0;
    sched_wait_queue_init(&child_out->state_waiters);

    if (process_clone_resources(child_out, parent, flags) != 0)
        return -1;

    if (flags & CLONE_SETTLS) {
        child_out->user_tls_base = tls;
        child_out->user_tls_limit = 0xFFFFFu;
        child_out->user_tls_limit_in_pages = 1;
        child_out->user_tls_present = 1;
    }

    if (process_clone_kernel_stack(child_out, parent, child_stack) != 0) {
        process_clone_rollback(child_out);
        return -1;
    }

    return 0;
}

void process_close_all_fds(process_t *proc)
{
    if (proc && proc->files)
        proc_fd_table_close_all(proc->files);
}

void process_release_user_space(process_t *proc)
{
    if (!proc || proc->pd_phys == 0)
        return;

    uint32_t *pd = (uint32_t *)proc->pd_phys;

    for (uint32_t pdi = 0; pdi < 1024; pdi++) {
        if ((pd[pdi] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
            continue;

            uint32_t *pt = (uint32_t *)paging_entry_addr(pd[pdi]);
        for (uint32_t pti = 0; pti < 1024; pti++) {
            if ((pt[pti] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
                continue;
            /*
             * Fork clones user mappings with copy-on-write, so a user frame may
             * still be shared with a live sibling or parent. Dropping this
             * address space must therefore release only this mapping's
             * reference, not unconditionally free the frame.
             */
            pmm_decref(paging_entry_addr(pt[pti]));
            pt[pti] = 0;
        }

        pmm_free_page((uint32_t)pt);
        pd[pdi] = 0;
    }

    pmm_free_page(proc->pd_phys);
    if (proc->as && proc->as->pd_phys == proc->pd_phys)
        proc->as->pd_phys = 0;
    proc->pd_phys = 0;
}

void process_release_kstack(process_t *proc)
{
    if (!proc || proc->kstack_bottom == 0)
        return;

    uint32_t raw   = proc->kstack_bottom;
    uint32_t guard = (raw + 0xFFFu) & ~0xFFFu;

    paging_unguard_page(guard);
    kfree((void *)raw);

    proc->kstack_bottom = 0;
    proc->kstack_top = 0;
    proc->saved_esp = 0;
}

void process_exec_cleanup(uint32_t old_pd_phys, uint32_t old_kstack_bottom)
{
    if (old_pd_phys != 0) {
        uint32_t *pd = (uint32_t *)old_pd_phys;

        for (uint32_t pdi = 0; pdi < 1024; pdi++) {
            if ((pd[pdi] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
                continue;

        uint32_t *pt = (uint32_t *)paging_entry_addr(pd[pdi]);
            for (uint32_t pti = 0; pti < 1024; pti++) {
                if ((pt[pti] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
                    continue;
                pmm_decref(paging_entry_addr(pt[pti]));
            }

            pmm_free_page((uint32_t)pt);
        }

        pmm_free_page(old_pd_phys);
    }

    if (old_kstack_bottom != 0) {
        uint32_t guard = (old_kstack_bottom + 0xFFFu) & ~0xFFFu;
        paging_unguard_page(guard);
        kfree((void *)old_kstack_bottom);
    }
}

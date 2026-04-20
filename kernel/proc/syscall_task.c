/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_task.c - Linux task, exec, and process-exit syscalls.
 *
 * This file owns execve, fork/vfork/clone, waitpid/wait4, thread-area setup,
 * set_tid_address, process/module image loading, and exit/exit_group.
 */

#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_linux.h"
#include "gdt.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "module.h"
#include "process.h"
#include "resources.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

static int linux_wait_child_matches(const process_t *cur,
                                    const process_t *child,
                                    int32_t selector)
{
	if (!cur || !child || child->parent_pid != cur->pid)
		return 0;
	if (selector == -1)
		return 1;
	if (selector == 0)
		return child->pgid == cur->pgid;
	if (selector < -1)
		return child->pgid == (uint32_t)(-selector);
	return child->pid == (uint32_t)selector;
}

static int
linux_wait_child(process_t *cur, uint32_t pid, int options, uint32_t *pid_out)
{
	int32_t selector = (int32_t)pid;

	if (!cur || !pid_out)
		return -1;
	*pid_out = 0;

	if (selector > 0) {
		const process_t *target = sched_find_process(pid, 1);
		int target_ready;
		int status;

		if (!linux_wait_child_matches(cur, target, selector))
			return -1;
		target_ready = target->state == PROC_ZOMBIE ||
		               ((options & WUNTRACED) && target->state == PROC_STOPPED);
		status = sched_waitpid(pid, options);
		if (status < 0)
			return status;
		if (status != 0 || target_ready || !(options & WNOHANG))
			*pid_out = pid;
		return status;
	}

	for (;;) {
		uint32_t pids[MAX_PROCS];
		int n = sched_snapshot_pids(pids, MAX_PROCS, 1);
		int found_child = 0;

		for (int i = 0; i < n; i++) {
			const process_t *child = sched_find_process(pids[i], 1);

			if (!linux_wait_child_matches(cur, child, selector))
				continue;
			found_child = 1;
			if (child->state == PROC_ZOMBIE ||
			    ((options & WUNTRACED) && child->state == PROC_STOPPED)) {
				uint32_t child_pid = child->pid;
				int status = sched_waitpid(child_pid, options);
				if (status >= 0)
					*pid_out = child_pid;
				return status;
			}
		}

		if (!found_child)
			return -1;
		if (options & WNOHANG)
			return 0;
		schedule();
	}
}

static uint32_t syscall_wait_common(uint32_t pid,
                                    uint32_t user_status,
                                    uint32_t options,
                                    uint32_t user_rusage)
{
	process_t *cur = sched_current();
	uint32_t waited_pid = pid;
	int status = linux_wait_child(cur, pid, (int)options, &waited_pid);

	if (status < 0)
		return (uint32_t)-1;
	if (waited_pid == 0)
		return 0;
	if (user_status != 0 &&
	    (!cur ||
	     uaccess_copy_to_user(cur, user_status, &status, sizeof(status)) != 0))
		return (uint32_t)-1;
	if (user_rusage != 0) {
		uint8_t zero[72];
		k_memset(zero, 0, sizeof(zero));
		if (!cur ||
		    uaccess_copy_to_user(cur, user_rusage, zero, sizeof(zero)) != 0)
			return (uint32_t)-1;
	}
	return waited_pid;
}

static int snapshot_user_string_vector(process_t *proc,
                                       uint32_t uvec,
                                       uint32_t max_count,
                                       uint32_t max_bytes,
                                       const char **out_vec,
                                       char *out_strs,
                                       int *out_count)
{
	uint32_t used = 0;

	*out_count = 0;
	if (uvec == 0) {
		out_vec[0] = 0;
		return 0;
	}

	for (uint32_t i = 0; i < max_count; i++) {
		uint32_t us = 0;
		uint32_t remaining;

		if (uaccess_copy_from_user(
		        proc, &us, uvec + i * sizeof(uint32_t), sizeof(uint32_t)) != 0)
			return -1;
		if (us == 0) {
			out_vec[i] = 0;
			*out_count = (int)i;
			return 0;
		}

		if (used >= max_bytes)
			return -1;
		remaining = max_bytes - used;
		out_vec[i] = &out_strs[used];
		if (uaccess_copy_string_from_user(
		        proc, &out_strs[used], remaining, us) != 0)
			return -1;
		used += k_strlen(&out_strs[used]) + 1;
	}

	return -1;
}

static uint32_t
syscall_execve(uint32_t user_path, uint32_t user_argv, uint32_t user_envp)
{
	const char *kargv[PROCESS_ARGV_MAX_COUNT + 1];
	char *kstrs = (char *)kmalloc(PROCESS_ARGV_MAX_BYTES);
	process_t *exec_cur = sched_current();
	int kargc = 0;
	const char *kenvp[PROCESS_ENV_MAX_COUNT + 1];
	char *kenvstrs;
	int kenvc = 0;
	char *exec_rpath;
	vfs_file_ref_t exec_ref;
	uint32_t sz;
	process_t *new_proc;

	if (!kstrs) {
		klog_uint("EXEC", "argv scratch heap free", kheap_free_bytes());
		return (uint32_t)-1;
	}
	kenvstrs = (char *)kmalloc(PROCESS_ENV_MAX_BYTES);
	if (!kenvstrs) {
		klog_uint("EXEC", "env scratch heap free", kheap_free_bytes());
		kfree(kstrs);
		return (uint32_t)-1;
	}

	if (!exec_cur) {
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}

	if (snapshot_user_string_vector(exec_cur,
	                                user_argv,
	                                PROCESS_ARGV_MAX_COUNT,
	                                PROCESS_ARGV_MAX_BYTES,
	                                kargv,
	                                kstrs,
	                                &kargc) != 0) {
		klog("EXEC", "bad argv");
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}
	if (snapshot_user_string_vector(exec_cur,
	                                user_envp,
	                                PROCESS_ENV_MAX_COUNT,
	                                PROCESS_ENV_MAX_BYTES,
	                                kenvp,
	                                kenvstrs,
	                                &kenvc) != 0) {
		klog("EXEC", "bad envp");
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}

	exec_rpath = syscall_alloc_path_scratch();
	if (!exec_rpath) {
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}
	if (resolve_user_path(exec_cur, user_path, exec_rpath, SYSCALL_PATH_MAX) !=
	    0) {
		klog("EXEC", "resolve path failed");
		kfree(exec_rpath);
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}
	if (vfs_open_file(exec_rpath, &exec_ref, &sz) != 0) {
		klog("EXEC", "file not found");
		klog("EXEC", exec_rpath);
		kfree(exec_rpath);
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}
	kfree(exec_rpath);

	new_proc = (process_t *)kmalloc(sizeof(process_t));
	if (!new_proc) {
		klog_uint("EXEC", "process descriptor heap free", kheap_free_bytes());
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}

	int create_rc = process_create_file(new_proc,
	                                    exec_ref,
	                                    kargv,
	                                    kargc,
	                                    kenvp,
	                                    kenvc,
	                                    proc_fd_entries(exec_cur));
	if (create_rc != 0) {
		klog_uint("EXEC", "process_create failed code", (uint32_t)(-create_rc));
		kfree(new_proc);
		kfree(kenvstrs);
		kfree(kstrs);
		return (uint32_t)-1;
	}
	for (uint32_t fd = 0; fd < MAX_FDS; fd++) {
		if (proc_fd_entries(new_proc)[fd].type != FD_TYPE_NONE &&
		    proc_fd_entries(new_proc)[fd].cloexec)
			fd_close_one(new_proc, fd);
	}
	kfree(kenvstrs);
	kfree(kstrs);

	new_proc->pid = exec_cur->pid;
	new_proc->parent_pid = exec_cur->parent_pid;
	new_proc->pgid = exec_cur->pgid;
	new_proc->sid = exec_cur->sid;
	new_proc->tty_id = exec_cur->tty_id;
	new_proc->state = PROC_RUNNING;
	new_proc->wait_queue = 0;
	new_proc->wait_next = 0;
	new_proc->wait_deadline = 0;
	new_proc->wait_deadline_set = 0;
	new_proc->exit_status = 0;
	new_proc->state_waiters = exec_cur->state_waiters;
	syscall_set_process_cwd(new_proc, syscall_process_cwd(exec_cur));

	new_proc->sig_pending = exec_cur->sig_pending;
	new_proc->sig_blocked = exec_cur->sig_blocked;
	for (int i = 0; i < NSIG; i++) {
		new_proc->sig_handlers[i] =
		    (exec_cur->sig_handlers[i] == SIG_IGN) ? SIG_IGN : SIG_DFL;
	}
	new_proc->crash.valid = 0;
	new_proc->crash.signum = 0;
	new_proc->crash.cr2 = 0;

	klog_hex("EXEC", "new_proc brk", new_proc->brk);
	klog_hex("EXEC", "new_proc heap_start", new_proc->heap_start);
	proc_resource_put_exec_owner(exec_cur);
	process_build_exec_frame(
	    new_proc, exec_cur->pd_phys, exec_cur->kstack_bottom);
	sched_exec_current(new_proc);
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_execve(uint32_t ebx,
                                              uint32_t ecx,
                                              uint32_t edx)
{
	{
		/*
         * ebx = pointer to null-terminated filename in user space
         * ecx = pointer to a user-space char *[] array (or 0 for no argv)
         * edx = pointer to a user-space envp[] array (or 0 for no env)
         *
         * Replace the calling process in-place.  PID, parent linkage,
         * process-group/session membership, cwd, and the open-fd table are
         * preserved; the user address space, user stack, heap, entry point,
         * and process metadata derived from argv are rebuilt from the new ELF.
         *
         * On success this syscall does not return to the old image.
         */
		process_t *cur = sched_current();
		if (cur && cur->group && task_group_live_count(cur->group) > 1)
			return (uint32_t)-1;

		return syscall_execve(ebx, ecx, edx);
	}
}

#define CLONE_EXIT_SIGNAL_MASK 0xFFu
#define CLONE_SUPPORTED_FLAGS                                                  \
	(CLONE_EXIT_SIGNAL_MASK | CLONE_VM | CLONE_FS | CLONE_FILES |              \
	 CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS | CLONE_PARENT_SETTID |       \
	 CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)

static int syscall_clone_validate_flags(uint32_t flags)
{
	uint32_t unsupported = flags & ~CLONE_SUPPORTED_FLAGS;
	if (unsupported) {
		klog_hex("CLONE", "unsupported flags", unsupported);
		return -1;
	}
	if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
		return -LINUX_EINVAL;
	if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
		return -LINUX_EINVAL;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_clone(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	process_t *parent = sched_current();
	process_t *child;
	int ctid;

	if (!parent)
		return (uint32_t)-1;
	{
		int rc = syscall_clone_validate_flags(ebx);
		if (rc != 0)
			return (uint32_t)rc;
	}

	child = (process_t *)kmalloc(sizeof(*child));
	if (!child)
		return (uint32_t)-1;

	if (process_clone(child, parent, ebx, ecx, edx, esi, edi) != 0) {
		kfree(child);
		return (uint32_t)-1;
	}

	if ((ebx & CLONE_PARENT_SETTID) && edx != 0) {
		uint32_t child_tid_preview = sched_peek_next_tid();
		if (uaccess_copy_to_user(
		        parent, edx, &child_tid_preview, sizeof(child_tid_preview)) !=
		    0) {
			process_clone_rollback(child);
			kfree(child);
			return (uint32_t)-1;
		}
	}

	ctid = sched_add(child);
	if (ctid < 0) {
		process_clone_rollback(child);
		kfree(child);
		return (uint32_t)-1;
	}
	kfree(child);

	if ((ebx & CLONE_CHILD_SETTID) && edi != 0) {
		process_t *slot = sched_find_pid((uint32_t)ctid);
		uint32_t tid_value = (uint32_t)ctid;

		if (!slot || uaccess_copy_to_user(
		                 slot, edi, &tid_value, sizeof(tid_value)) != 0) {
			sched_force_remove_task((uint32_t)ctid);
			return (uint32_t)-1;
		}
	}

	return (uint32_t)ctid;
}

uint32_t SYSCALL_NOINLINE syscall_case_fork_vfork(void)
{
	{
		/*
         * fork() takes no arguments.  vfork() is implemented as fork() for
         * compatibility; copy-on-write keeps the common fork+exec path cheap.
         *
         * Creates a child process that is an exact copy of the caller's
         * address space, registers, and open-file table.  The child's
         * fork() returns 0; the parent's fork() returns the child's PID.
         * Both processes resume execution at the instruction after INT 0x80.
         *
         * Implemented via copy-on-write user mappings, with the live user
         * stack eagerly copied so pre-exec child activity cannot mutate the
         * parent's active stack pages. See process_fork() in process.c and
         * paging_clone_user_space() in paging.c.
         *
         * Returns child PID in parent, 0 in child, (uint32_t)-1 on error.
         */
		process_t *parent = sched_current();
		if (!parent)
			return (uint32_t)-1;

		/* Allocate child descriptor on the heap: process_t is ~5 KB. */
		process_t *child = (process_t *)kmalloc(sizeof(process_t));
		if (!child) {
			klog_uint("FORK", "heap free bytes", kheap_free_bytes());
			return (uint32_t)-1;
		}

		if (process_fork(child, parent) != 0) {
			klog("FORK", "process_fork failed");
			kfree(child);
			return (uint32_t)-1;
		}

		int cpid = sched_add(child);
		kfree(child); /* sched_add copies by value into proc_table[] */
		if (cpid < 0) {
			klog("FORK", "process table full");
			return (uint32_t)-1;
		}
		return (uint32_t)
		    cpid; /* parent gets child PID; child frame already has EAX=0 */
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_set_thread_area(uint32_t ebx)
{
	{
		/*
         * ebx = struct user_desc *.
         *
         * Static i386 musl uses set_thread_area during startup to install a
         * TLS descriptor and then loads %gs with the returned entry number.
         * Drunix exposes one user TLS slot in the GDT for this compatibility
         * path.
         */
		process_t *cur = sched_current();
		linux_user_desc_t desc;
		uint32_t contents;
		int limit_in_pages;

		if (!cur || ebx == 0)
			return (uint32_t)-1;
		if (uaccess_copy_from_user(cur, &desc, ebx, sizeof(desc)) != 0)
			return (uint32_t)-1;

		if (desc.entry_number != 0xFFFFFFFFu &&
		    desc.entry_number != GDT_USER_TLS_ENTRY)
			return (uint32_t)-LINUX_EINVAL;

		contents = (desc.flags >> 1) & 0x3u;
		if (contents != 0)
			return (uint32_t)-LINUX_EINVAL;
		if ((desc.flags & (1u << 3)) != 0)
			return (uint32_t)-LINUX_EINVAL;

		desc.entry_number = GDT_USER_TLS_ENTRY;
		limit_in_pages = (desc.flags & (1u << 4)) != 0;
		if ((desc.flags & (1u << 5)) != 0) {
			cur->user_tls_base = 0;
			cur->user_tls_limit = 0;
			cur->user_tls_limit_in_pages = 0;
			cur->user_tls_present = 0;
		} else {
			cur->user_tls_base = desc.base_addr;
			cur->user_tls_limit = desc.limit;
			cur->user_tls_limit_in_pages = (uint32_t)limit_in_pages;
			cur->user_tls_present = 1;
		}
		process_restore_user_tls(cur);

		if (uaccess_copy_to_user(cur, ebx, &desc, sizeof(desc)) != 0)
			return (uint32_t)-1;
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_set_tid_address(void)
{
	{
		/*
         * ebx = int *tidptr.
         * A single-threaded runtime only needs the Linux return contract:
         * return the caller's thread id, which is the process id in Drunix.
         */
		process_t *cur = sched_current();

		if (!cur)
			return (uint32_t)-1;
		return cur->pid;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_modload(uint32_t ebx)
{
	{
		/*
         * ebx = pointer to null-terminated module filename in user space.
         *
         * Looks up the file via the VFS to get a mount-qualified file ref and
         * size, then calls module_load_file() to read the ELF relocatable object
         * from disk, resolve symbols against kernel_exports[], apply
         * relocations, and call the module's module_init() function.
         *
         * Returns 0 on success, or a negative error code from module_load_file():
         *   -1  invalid ELF
         *   -2  relocation error (undefined symbol or unsupported reloc type)
         *   -3  out of kernel heap memory
         *   -4  module_init() returned non-zero
         *   -5  module too large (> MODULE_MAX_SIZE)
         */
		process_t *cur = sched_current();
		char *rpath = syscall_alloc_path_scratch();
		if (!rpath)
			return (uint32_t)-1;
		if (!cur || resolve_user_path(cur, ebx, rpath, SYSCALL_PATH_MAX) != 0) {
			kfree(rpath);
			return (uint32_t)-1;
		}
		vfs_file_ref_t mod_ref;
		uint32_t sz;
		if (vfs_open_file(rpath, &mod_ref, &sz) != 0) {
			klog("MODLOAD", "file not found");
			kfree(rpath);
			return (uint32_t)-1;
		}
		{
			uint32_t ret = (uint32_t)module_load_file(rpath, mod_ref, sz);
			kfree(rpath);
			return ret;
		}
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_exit_exit_group(uint32_t eax,
                                                       uint32_t ebx)
{
	/*
         * ebx = exit status code.
         * Store the exit code, mark the process as a zombie, and switch
         * away.  schedule() never returns here — the zombie's kernel stack
         * is abandoned and will be freed when the slot is reused.
         */
	if (eax == SYS_EXIT_GROUP) {
		sched_mark_group_exit(ebx);
	} else {
		sched_set_exit_status(ebx);
		sched_mark_exit();
	}
	schedule();
	__builtin_unreachable();
}

uint32_t SYSCALL_NOINLINE syscall_case_waitpid(uint32_t ebx,
                                               uint32_t ecx,
                                               uint32_t edx)
{
	{
		/*
         * ebx = pid, ecx = int *status, edx = options.
         * Writes Linux-style encoded status:
         *   Exited:  (exit_code << 8)          — low 7 bits == 0
         *   Stopped: (stop_signal << 8) | 0x7F — low 7 bits == 0x7F
         * Returns pid, 0 with WNOHANG, or -1 for no such process / bad status.
        */
		return syscall_wait_common(ebx, ecx, edx, 0);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_wait4(uint32_t ebx,
                                             uint32_t ecx,
                                             uint32_t edx,
                                             uint32_t esi)
{
	{
		/*
         * Linux i386 wait4(pid, status, options, rusage).  Resource usage is
         * not tracked yet; the wait/reap semantics match waitpid.
        */
		return syscall_wait_common(ebx, ecx, edx, esi);
	}
}

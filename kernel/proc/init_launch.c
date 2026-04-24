/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "init_launch.h"

#include "kheap.h"
#include "kprintf.h"
#include "klog.h"
#include "desktop.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"

static const char *boot_launch_kind_label(int launch_mode_flag)
{
	/* The flag controls whether the helper attaches the launched PID to the
	 * active desktop after PID creation. */
	return launch_mode_flag ? "desktop-attached" : "standalone";
}

int boot_launch_init_process(const char *path,
                             const char *arg0,
                             const char *env0,
                             int attach_desktop_pid)
{
	vfs_file_ref_t file_ref;
	uint32_t elf_size = 0;
	process_t init_proc;
	const char *argv[1];
	const char *envp[1];
	const int launch_mode_flag = attach_desktop_pid;
	const char *launch_kind = boot_launch_kind_label(launch_mode_flag);
	char log_line[96];
	int rc;

	argv[0] = arg0;
	envp[0] = env0;
	k_snprintf(log_line, sizeof(log_line), "%s launch: locating initial program", launch_kind);
	klog("BOOT", log_line);
	if (vfs_open_file(path, &file_ref, &elf_size) != 0) {
		k_snprintf(log_line, sizeof(log_line), "%s launch: initial program not found", launch_kind);
		klog("FS", log_line);
		return BOOT_LAUNCH_INIT_ERR_NOT_FOUND;
	}
	k_snprintf(log_line, sizeof(log_line), "%s launch: initial program inode", launch_kind);
	klog_uint("FS", log_line, file_ref.inode_num);
	k_snprintf(log_line, sizeof(log_line), "%s launch: initial program size", launch_kind);
	klog_uint("FS", log_line, elf_size);
	k_snprintf(log_line, sizeof(log_line), "%s launch: before process_create", launch_kind);
	klog_uint("HEAP", log_line, kheap_free_bytes());
	rc = process_create_file(&init_proc, file_ref, argv, 1, envp, 1, 0);
	k_snprintf(log_line, sizeof(log_line), "%s launch: after process_create", launch_kind);
	klog_uint("HEAP", log_line, kheap_free_bytes());
	if (rc != 0) {
		k_snprintf(log_line, sizeof(log_line), "%s launch: process_create failed, code", launch_kind);
		klog_uint("PROC", log_line, (uint32_t)(-rc));
		return BOOT_LAUNCH_INIT_ERR_PROCESS_CREATE;
	}
	if (sched_add(&init_proc) < 0) {
		k_snprintf(log_line, sizeof(log_line), "%s launch: sched_add failed", launch_kind);
		klog("PROC", log_line);
		return BOOT_LAUNCH_INIT_ERR_SCHED_ADD;
	}
	if (launch_mode_flag && desktop_is_active()) {
		desktop_state_t *desktop = desktop_global();

		if (desktop) {
			desktop_attach_shell_pid(desktop, (uint32_t)init_proc.pid);
			desktop_render(desktop);
		}
	}
	return (int)init_proc.pid;
}

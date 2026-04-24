/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "init_launch.h"

#include "kheap.h"
#include "kprintf.h"
#include "klog.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"

static const char *boot_launch_kind_label(int attach_desktop_pid)
{
	return attach_desktop_pid ? "desktop-attached" : "standalone";
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
	const char *launch_kind = boot_launch_kind_label(attach_desktop_pid);
	char log_line[96];
	int rc;

	argv[0] = arg0;
	envp[0] = env0;
	k_snprintf(log_line, sizeof(log_line), "%s launch: locating initial program", launch_kind);
	klog("BOOT", log_line);
	if (vfs_open_file(path, &file_ref, &elf_size) != 0) {
		k_snprintf(log_line, sizeof(log_line), "%s launch: initial program not found", launch_kind);
		klog("FS", log_line);
		return -1;
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
		return rc;
	}
	if (sched_add(&init_proc) < 0) {
		k_snprintf(log_line, sizeof(log_line), "%s launch: sched_add failed", launch_kind);
		klog("PROC", log_line);
		return -2;
	}
	return (int)init_proc.pid;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "init_launch.h"

#include "kheap.h"
#include "klog.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"

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
	int rc;

	argv[0] = arg0;
	envp[0] = env0;
	klog("BOOT", "locating initial program");
	if (attach_desktop_pid)
		klog("BOOT", "desktop attach requested for initial process");
	if (vfs_open_file(path, &file_ref, &elf_size) != 0) {
		klog("FS", "initial program not found");
		return -1;
	}
	klog_uint("FS", "initial program inode", file_ref.inode_num);
	klog_uint("FS", "initial program size", elf_size);
	klog_uint("HEAP", "before process_create", kheap_free_bytes());
	rc = process_create_file(&init_proc, file_ref, argv, 1, envp, 1, 0);
	klog_uint("HEAP", "after process_create", kheap_free_bytes());
	if (rc != 0) {
		klog_uint("PROC", "process_create failed, code", (uint32_t)(-rc));
		return rc;
	}
	if (sched_add(&init_proc) < 0) {
		klog("PROC", "sched_add failed");
		return -2;
	}
	return (int)init_proc.pid;
}

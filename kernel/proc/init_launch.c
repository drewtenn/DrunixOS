/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "init_launch.h"

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
	static process_t init_proc;
	static const char *argv[1];
	static const char *envp[1];
	int rc;

	(void)attach_desktop_pid;

	argv[0] = arg0;
	envp[0] = env0;
	if (vfs_open_file(path, &file_ref, &elf_size) != 0) {
		klog("BOOT", "initial program not found");
		return -1;
	}
	rc = process_create_file(&init_proc, file_ref, argv, 1, envp, 1, 0);
	if (rc != 0)
		return rc;
	if (sched_add(&init_proc) < 0)
		return -2;
	return (int)init_proc.pid;
}

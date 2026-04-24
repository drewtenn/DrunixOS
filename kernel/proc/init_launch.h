/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef INIT_LAUNCH_H
#define INIT_LAUNCH_H

/* The fourth argument is a launch-mode flag: when set, the helper attaches
 * the launched PID to the active desktop after PID creation. */
#define BOOT_LAUNCH_INIT_ERR_NOT_FOUND (-1)
#define BOOT_LAUNCH_INIT_ERR_PROCESS_CREATE (-2)
#define BOOT_LAUNCH_INIT_ERR_SCHED_ADD (-3)

int boot_launch_init_process(const char *path,
                             const char *arg0,
                             const char *env0,
                             int attach_desktop_pid);

#endif /* INIT_LAUNCH_H */

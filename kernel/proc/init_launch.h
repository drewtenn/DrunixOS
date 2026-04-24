/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef INIT_LAUNCH_H
#define INIT_LAUNCH_H

int boot_launch_init_process(const char *path,
                             const char *arg0,
                             const char *env0,
                             int attach_desktop_pid);

#endif /* INIT_LAUNCH_H */

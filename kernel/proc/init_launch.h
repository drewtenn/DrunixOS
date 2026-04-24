/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef INIT_LAUNCH_H
#define INIT_LAUNCH_H

/* Classifies the shared boot launch path for diagnostics; the caller still
 * handles any desktop attachment. */
int boot_launch_init_process(const char *path,
                             const char *arg0,
                             const char *env0,
                             int attach_desktop_pid);

#endif /* INIT_LAUNCH_H */

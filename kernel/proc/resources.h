/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PROC_RESOURCES_H
#define PROC_RESOURCES_H

#include "process.h"

int proc_resource_init_fresh(process_t *proc);
int proc_resource_clone_for_fork(process_t *child, const process_t *parent);
void proc_resource_mirror_from_process(process_t *proc);
void proc_resource_get_all(process_t *proc);
void proc_resource_put_all(process_t *proc);
void proc_resource_put_files(process_t *proc);
void proc_resource_put_exec_nonfiles(process_t *proc);
void proc_resource_put_exec_owner(process_t *proc);
void proc_fd_table_close_all(proc_fd_table_t *files);
int proc_fd_table_dup(proc_fd_table_t **out, const proc_fd_table_t *src);
int proc_fs_state_dup(proc_fs_state_t **out, const proc_fs_state_t *src);
int proc_sig_actions_dup(proc_sig_actions_t **out,
                         const proc_sig_actions_t *src);

#endif

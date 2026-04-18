/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TASK_GROUP_H
#define TASK_GROUP_H

#include "wait.h"
#include <stdint.h>

typedef struct task_group {
    uint32_t used;
    uint32_t refs;
    uint32_t tgid;
    uint32_t leader_tid;
    uint32_t parent_tgid;
    uint32_t pgid;
    uint32_t sid;
    uint32_t tty_id;
    uint32_t live_tasks;
    uint32_t exit_signal;
    uint32_t group_exit;
    uint32_t exit_status;
    uint32_t sig_pending;
    wait_queue_t state_waiters;
} task_group_t;

void task_group_table_init(void);
task_group_t *task_group_create(uint32_t tgid, uint32_t leader_tid,
                                uint32_t parent_tgid, uint32_t pgid,
                                uint32_t sid, uint32_t tty_id,
                                uint32_t exit_signal);
void task_group_get(task_group_t *group);
void task_group_put(task_group_t *group);
void task_group_add_task(task_group_t *group);
void task_group_remove_task(task_group_t *group);
uint32_t task_group_tgid(const task_group_t *group);
uint32_t task_group_leader_tid(const task_group_t *group);
uint32_t task_group_live_count(const task_group_t *group);
void task_group_set_process_signal(task_group_t *group, int signum);
uint32_t task_group_take_process_signal(task_group_t *group, uint32_t blocked);

#endif

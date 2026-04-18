/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "task_group.h"
#include "sched.h"
#include "kstring.h"

static task_group_t groups[MAX_PROCS];

void task_group_table_init(void)
{
    k_memset(groups, 0, sizeof(groups));
}

task_group_t *task_group_create(uint32_t tgid, uint32_t leader_tid,
                                uint32_t parent_tgid, uint32_t pgid,
                                uint32_t sid, uint32_t tty_id,
                                uint32_t exit_signal)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!groups[i].used) {
            task_group_t *group = &groups[i];
            k_memset(group, 0, sizeof(*group));
            group->used = 1;
            group->refs = 1;
            group->tgid = tgid;
            group->leader_tid = leader_tid;
            group->parent_tgid = parent_tgid;
            group->pgid = pgid;
            group->sid = sid;
            group->tty_id = tty_id;
            group->exit_signal = exit_signal;
            sched_wait_queue_init(&group->state_waiters);
            return group;
        }
    }
    return 0;
}

void task_group_get(task_group_t *group)
{
    if (group)
        group->refs++;
}

void task_group_put(task_group_t *group)
{
    if (!group || group->refs == 0)
        return;
    group->refs--;
    if (group->refs == 0)
        k_memset(group, 0, sizeof(*group));
}

void task_group_add_task(task_group_t *group)
{
    if (group)
        group->live_tasks++;
}

void task_group_remove_task(task_group_t *group)
{
    if (group && group->live_tasks > 0)
        group->live_tasks--;
}

uint32_t task_group_tgid(const task_group_t *group)
{
    return group ? group->tgid : 0;
}

uint32_t task_group_leader_tid(const task_group_t *group)
{
    return group ? group->leader_tid : 0;
}

uint32_t task_group_live_count(const task_group_t *group)
{
    return group ? group->live_tasks : 0;
}

void task_group_set_process_signal(task_group_t *group, int signum)
{
    if (!group || signum < 1 || signum >= NSIG)
        return;
    group->sig_pending |= (1u << signum);
}

uint32_t task_group_take_process_signal(task_group_t *group, uint32_t blocked)
{
    uint32_t deliverable;

    if (!group)
        return 0;

    deliverable = group->sig_pending & ~blocked;
    for (int i = 1; i < NSIG; i++) {
        uint32_t bit = 1u << i;
        if (deliverable & bit) {
            group->sig_pending &= ~bit;
            return (uint32_t)i;
        }
    }
    return 0;
}

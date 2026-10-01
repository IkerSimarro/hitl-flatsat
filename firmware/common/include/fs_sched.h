/*
** Cooperative time-triggered scheduler
**
** Each task runs at a fixed period, with an optional phase offset to spread work across the cycle.
** Tasks run to completion, in table order when several are due, so behaviour is deterministic and
** identical on Linux and the Pico. The scheduler measures per-task execution time, overruns (a task
** still due a full period after it should have run) and overall CPU load for OBC_HK.
*/
#ifndef FS_SCHED_H
#define FS_SCHED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fs_task_fn)(void);

typedef struct
{
    const char *name;
    uint32_t    period_ms;
    uint32_t    offset_ms;
    fs_task_fn  fn;

    /* Maintained by the scheduler */
    uint64_t next_due_us;
    uint32_t runs;
    uint32_t overruns;
    uint32_t last_exec_us;
    uint32_t max_exec_us;
} fs_task_t;

/* Use the caller's task table (not copied); start times are counted from now */
void fs_sched_init(fs_task_t *tasks, unsigned count);

/* Runs every task that is due; returns microseconds until the next task is due */
uint32_t fs_sched_run(void);

/* Busy time as a percentage of the last completed 1 s window */
uint8_t fs_sched_cpu_load(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_SCHED_H */

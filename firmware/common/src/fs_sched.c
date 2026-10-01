/*
** Cooperative time-triggered scheduler (see fs_sched.h)
*/
#include "fs_sched.h"

#include "fs_hal.h"

#define LOAD_WINDOW_US 1000000u

static fs_task_t *table;
static unsigned   table_count;

static uint64_t window_start_us;
static uint64_t window_busy_us;
static uint8_t  cpu_load;

void fs_sched_init(fs_task_t *tasks, unsigned count)
{
    uint64_t now = fs_hal_time_us();
    unsigned i;

    table       = tasks;
    table_count = count;
    for (i = 0; i < count; i++)
    {
        tasks[i].next_due_us  = now + (uint64_t)tasks[i].offset_ms * 1000u;
        tasks[i].runs         = 0;
        tasks[i].overruns     = 0;
        tasks[i].last_exec_us = 0;
        tasks[i].max_exec_us  = 0;
    }
    window_start_us = now;
    window_busy_us  = 0;
    cpu_load        = 0;
}

static void account_load(uint64_t now)
{
    if (now - window_start_us >= LOAD_WINDOW_US)
    {
        uint64_t pct = window_busy_us * 100u / (now - window_start_us);
        cpu_load        = (uint8_t)(pct > 100u ? 100u : pct);
        window_start_us = now;
        window_busy_us  = 0;
    }
}

uint32_t fs_sched_run(void)
{
    uint64_t now = fs_hal_time_us();
    uint64_t next_wake;
    unsigned i;

    for (i = 0; i < table_count; i++)
    {
        fs_task_t *t = &table[i];
        uint64_t   start;
        uint64_t   period_us;

        if (now < t->next_due_us)
        {
            continue;
        }

        start = fs_hal_time_us();
        t->fn();
        now = fs_hal_time_us();

        t->last_exec_us = (uint32_t)(now - start);
        if (t->last_exec_us > t->max_exec_us)
        {
            t->max_exec_us = t->last_exec_us;
        }
        t->runs++;
        window_busy_us += now - start;

        /* Keep a fixed cadence; if a whole period was missed, count an overrun and resynchronise
         * rather than running the task back-to-back to catch up */
        period_us = (uint64_t)t->period_ms * 1000u;
        t->next_due_us += period_us;
        if (now >= t->next_due_us)
        {
            t->overruns++;
            t->next_due_us = now + period_us;
        }
    }

    account_load(now);

    next_wake = UINT64_MAX;
    for (i = 0; i < table_count; i++)
    {
        if (table[i].next_due_us < next_wake)
        {
            next_wake = table[i].next_due_us;
        }
    }
    if (next_wake == UINT64_MAX || next_wake <= now)
    {
        return 0;
    }
    return (uint32_t)((next_wake - now) > UINT32_MAX ? UINT32_MAX : (next_wake - now));
}

uint8_t fs_sched_cpu_load(void)
{
    return cpu_load;
}

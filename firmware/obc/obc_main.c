/*
** OBC flight software: initialisation and main loop
**
** The umbilical is serviced on every loop pass; everything else runs from the time-triggered scheduler.
** Linux (software-in-the-loop): obc --umb-pty PATH [--can IFACE]
*/
#include <string.h>

#include "fs_hal.h"
#include "fs_sched.h"
#include "fs_time.h"
#include "fs_umbilical.h"
#include "obc.h"

/* Mission start time (nos3-mission.xml start-time), used until the first TIME frame arrives */
#define MISSION_EPOCH_S 814254200u

/* Longest sleep between loop passes, so received frames are handled promptly */
#define MAX_IDLE_US 2000u

obc_state_t obc;

static int link_was_up;

/* ---- Umbilical handlers (no bus access allowed here, see fs_umbilical.h) ---- */

static void on_command(const uint8_t *pkt, size_t len)
{
    obc_cmd_enqueue(pkt, len);
}

static void on_time(fs_time_t sim_time)
{
    fs_time_sync(sim_time, 1);
}

/* ---- Tasks ---- */

static void task_commands(void)
{
    obc_cmd_process();
}

static void task_heartbeat(void)
{
    int up = fs_umb_link_up();

    fs_umb_heartbeat();
    fs_hal_watchdog_kick();
    if (up != link_was_up)
    {
        link_was_up = up;
        obc_event(EVT_UMBILICAL_LINK, up ? FLATSAT_SEVERITY_INFO : FLATSAT_SEVERITY_WARNING, "umbilical link %s",
                  up ? "up" : "down");
    }
}

static void task_sensors(void)
{
    obc_sensors_acquire();
}

static void task_gps(void)
{
    obc_sensors_poll_gps();
}

static void task_telemetry(void)
{
    obc_tlm_service();
}

static fs_task_t tasks[] = {
    {.name = "commands", .period_ms = 50, .offset_ms = 0, .fn = task_commands},
    {.name = "heartbeat", .period_ms = 250, .offset_ms = 10, .fn = task_heartbeat},
    {.name = "gps", .period_ms = 100, .offset_ms = 20, .fn = task_gps},
    {.name = "sensors", .period_ms = 1000, .offset_ms = 100, .fn = task_sensors},
    {.name = "telemetry", .period_ms = 50, .offset_ms = 30, .fn = task_telemetry},
};

int main(int argc, char **argv)
{
    fs_umb_handlers_t handlers = {.on_command = on_command, .on_time = on_time};
    fs_time_t         epoch    = {MISSION_EPOCH_S, 0};

    if (fs_hal_init(argc, argv) != 0)
    {
        fs_hal_log("HAL initialisation failed");
        return 1;
    }

    memset(&obc, 0, sizeof(obc));
    obc.reset_cause = fs_hal_reset_cause();
    obc.bdot_gain   = 0.0f;

    fs_time_init(epoch);
    fs_umb_init(&handlers);
    obc_mode_init();
    obc_cmd_init();
    obc_tlm_init();
    obc_sensors_init();
    fs_sched_init(tasks, sizeof(tasks) / sizeof(tasks[0]));

    fs_hal_log("OBC flight software started, ICD %s, reset cause %u", FLATSAT_ICD_VERSION, obc.reset_cause);

    for (;;)
    {
        uint32_t wait_us;

        fs_umb_poll();
        wait_us = fs_sched_run();
        fs_hal_sleep_us(wait_us < MAX_IDLE_US ? wait_us : MAX_IDLE_US);
    }
}

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
    obc_cmd_enqueue(pkt, len, OBC_CMD_SRC_UMB);
}

static void on_rf_frame(const uint8_t *frame, size_t len, int16_t rssi, int8_t snr)
{
    obc_comms_on_rx(frame, len, rssi, snr);
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

static void task_comms(void)
{
    obc_comms_task();
}

static void task_adcs(void)
{
    obc_adcs_step();
}

static void task_adcs_auto(void)
{
    obc_adcs_auto();
}

static void task_gps(void)
{
    obc_sensors_poll_gps();
}

static void task_telemetry(void)
{
    obc_tlm_service();
}

static void task_can_fast(void)
{
    obc_can_task_fast();
}

static void task_can_slow(void)
{
    obc_can_task_slow();
}

static fs_task_t tasks[] = {
    {.name = "commands", .period_ms = 50, .offset_ms = 0, .fn = task_commands},
    {.name = "heartbeat", .period_ms = 250, .offset_ms = 10, .fn = task_heartbeat},
    {.name = "gps", .period_ms = 100, .offset_ms = 20, .fn = task_gps},
    {.name = "sensors", .period_ms = 1000, .offset_ms = 100, .fn = task_sensors},
    {.name = "telemetry", .period_ms = 50, .offset_ms = 30, .fn = task_telemetry},
    {.name = "can_fast", .period_ms = 100, .offset_ms = 40, .fn = task_can_fast},
    {.name = "can_slow", .period_ms = 1000, .offset_ms = 60, .fn = task_can_slow},
    {.name = "adcs", .period_ms = 200, .offset_ms = 70, .fn = task_adcs},
    {.name = "comms", .period_ms = 100, .offset_ms = 90, .fn = task_comms},
    {.name = "adcs_auto", .period_ms = 1000, .offset_ms = 180, .fn = task_adcs_auto},
};

int main(int argc, char **argv)
{
    fs_umb_handlers_t handlers = {.on_command = on_command, .on_time = on_time, .on_rf_frame = on_rf_frame};
    fs_time_t         epoch    = {MISSION_EPOCH_S, 0};

    if (fs_hal_init(argc, argv) != 0)
    {
        fs_hal_log("HAL initialisation failed");
        return 1;
    }

    memset(&obc, 0, sizeof(obc));
    obc.reset_cause = fs_hal_reset_cause();

    fs_time_init(epoch);
    fs_umb_init(&handlers);
    obc_mode_init();
    obc_cmd_init();
    obc_tlm_init();
    obc_sensors_init();
    obc_adcs_init();
    obc_comms_init();
    obc_can_init();
    fs_sched_init(tasks, sizeof(tasks) / sizeof(tasks[0]));

    fs_hal_log("OBC flight software started, ICD %s, reset cause %u", FLATSAT_ICD_VERSION, obc.reset_cause);

    for (;;)
    {
        uint32_t wait_us;

        fs_umb_poll();
        obc_can_poll();
        wait_us = fs_sched_run();
        fs_hal_sleep_us(wait_us < MAX_IDLE_US ? wait_us : MAX_IDLE_US);
    }
}

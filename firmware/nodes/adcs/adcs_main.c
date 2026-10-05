/*
** ADCS actuator node: drives the physical reaction wheel for the OBC over CAN (ICD 6)
**
** - RW_CMD (OBC, 10 Hz): wheel speed (rpm) or duty set point; the speed loop runs at 100 Hz
** - RW_TLM (10 Hz): measured speed, set point echo, duty, status flags, command sequence echo
** - no RW_CMD for 1 s, OBC heartbeat lost, or ENTER_SAFE: wheel off, FAULT or SAFE state reported
** Linux (SIL): adcs_node [--can vcan0] [--run-line adcs]
*/
#include <math.h>
#include <string.h>

#include "dev_motor.h"
#include "flatsat_icd.h"
#include "fs_can.h"
#include "fs_hal.h"
#include "fs_node.h"
#include "fs_sched.h"
#include "fs_time.h"
#include "wheel_ctrl.h"

#define CONTROL_PERIOD_MS 10
#define TLM_PERIOD_MS     100
#define CMD_TIMEOUT_US    1000000u
#define PHYS_WHEEL        0

/* Speed loop tuning, for the plant model's N20 and flywheel (0.25 s time constant, ~650 rpm at duty 1) */
#define KP            0.0008f /* duty per rpm */
#define KI            0.004f  /* duty per rpm per s */
#define RPM_PER_DUTY  640.0f
#define SPEED_FILTER  0.3f
#define INTEGRAL_BAND 50.0f /* rpm */
#define AT_SETPOINT_RPM 20.0f

/* RW_TLM.STATUS bits */
#define ST_DRIVER_ENABLED 0x01
#define ST_AT_SETPOINT    0x02
#define ST_SATURATED      0x04
#define ST_CMD_TIMEOUT    0x08
#define ST_SAFE           0x10

/* FAULT.CODE values sent by this node */
#define FAULT_CMD_TIMEOUT     1
#define FAULT_DRIVER_DISABLED 2

static wheel_ctrl_t ctrl;
static uint8_t      ctrl_mode = FLATSAT_RW_CTRL_MODE_OFF;
static int16_t      setpoint; /* rpm, or 0.01 % duty in DUTY mode */
static uint8_t      cmd_seq;
static uint64_t     last_cmd_us;
static int          cmd_timed_out;
static int          safe = 1; /* until the OBC commands the wheel */
static int          driver_was_enabled = 1;

static void send_fault(uint8_t code, uint16_t value)
{
    flatsat_can_fault_t f;

    memset(&f, 0, sizeof(f));
    f.code  = code;
    f.value = value;
    fs_can_send(FLATSAT_CAN_FAULT_TYPE, &f);
}

static void stop_wheel(void)
{
    ctrl_mode = FLATSAT_RW_CTRL_MODE_OFF;
    wheel_ctrl_reset(&ctrl);
    dev_motor_set_duty(0.0f);
}

/* ---- Node callbacks ---- */

static void enter_safe(const char *reason)
{
    fs_hal_log("safe state: %s", reason);
    safe = 1;
    stop_wheel();
}

static uint8_t node_state(void)
{
    if (safe)
    {
        return FLATSAT_NODE_STATE_SAFE;
    }
    return cmd_timed_out || !dev_motor_driver_enabled() ? FLATSAT_NODE_STATE_FAULT : FLATSAT_NODE_STATE_NOMINAL;
}

static void on_rw_cmd(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_rw_cmd_t *c = payload;

    (void)dlc;
    if (src != FLATSAT_NODE_OBC || c->wheel != PHYS_WHEEL)
    {
        return;
    }
    last_cmd_us   = fs_hal_time_us();
    cmd_seq       = c->seq;
    cmd_timed_out = 0;
    if (c->ctrl_mode != FLATSAT_RW_CTRL_MODE_OFF)
    {
        safe = 0; /* the OBC is commanding the wheel again */
    }
    if (safe)
    {
        return;
    }
    if (c->ctrl_mode != ctrl_mode)
    {
        wheel_ctrl_reset(&ctrl);
    }
    ctrl_mode = c->ctrl_mode;
    setpoint  = c->setpoint;
}

/* ---- Tasks ---- */

static void task_control(void)
{
    float dt = CONTROL_PERIOD_MS / 1000.0f;
    int   enabled;

    fs_hal_check_power(); /* doesn't return while the EPS node holds us in reset */
    wheel_ctrl_update_speed(&ctrl, dev_motor_encoder_count(), dt);

    if (!safe && ctrl_mode != FLATSAT_RW_CTRL_MODE_OFF && fs_hal_time_us() - last_cmd_us > CMD_TIMEOUT_US)
    {
        cmd_timed_out = 1;
        stop_wheel();
        send_fault(FAULT_CMD_TIMEOUT, 0);
        fs_hal_log("no RW_CMD for %u ms: wheel stopped", CMD_TIMEOUT_US / 1000u);
    }

    enabled = dev_motor_driver_enabled();
    if (!enabled && driver_was_enabled)
    {
        send_fault(FAULT_DRIVER_DISABLED, 0);
        fs_hal_log("motor driver disabled (nSLEEP low)");
    }
    driver_was_enabled = enabled;

    switch (ctrl_mode)
    {
        case FLATSAT_RW_CTRL_MODE_SPEED:
            dev_motor_set_duty(wheel_ctrl_step(&ctrl, (float)setpoint, dt));
            break;
        case FLATSAT_RW_CTRL_MODE_DUTY:
            ctrl.duty = (float)setpoint / 10000.0f;
            dev_motor_set_duty(ctrl.duty);
            break;
        default:
            dev_motor_set_duty(0.0f);
            break;
    }
}

static void task_telemetry(void)
{
    flatsat_can_rw_tlm_t t;
    float                err = (float)setpoint - ctrl.speed_rpm;

    memset(&t, 0, sizeof(t));
    t.wheel         = PHYS_WHEEL;
    t.speed         = (int16_t)lroundf(ctrl.speed_rpm);
    t.setpoint_echo = ctrl_mode == FLATSAT_RW_CTRL_MODE_OFF ? 0 : setpoint;
    t.duty          = (int8_t)lroundf(ctrl.duty * 100.0f);
    t.seq_echo      = cmd_seq;
    t.status        = (uint8_t)((dev_motor_driver_enabled() ? ST_DRIVER_ENABLED : 0) |
                         (ctrl_mode == FLATSAT_RW_CTRL_MODE_SPEED && fabsf(err) < AT_SETPOINT_RPM ? ST_AT_SETPOINT : 0) |
                         (ctrl.saturated ? ST_SATURATED : 0) | (cmd_timed_out ? ST_CMD_TIMEOUT : 0) |
                         (safe ? ST_SAFE : 0));
    fs_can_send(FLATSAT_CAN_RW_TLM_TYPE, &t);
}

static fs_task_t tasks[] = {
    {.name = "control", .period_ms = CONTROL_PERIOD_MS, .offset_ms = 0, .fn = task_control},
    {.name = "telemetry", .period_ms = TLM_PERIOD_MS, .offset_ms = 5, .fn = task_telemetry},
};

int main(int argc, char **argv)
{
    fs_node_callbacks_t cbs   = {.enter_safe = enter_safe, .state = node_state};
    fs_time_t           epoch = {814254200u, 0};

    if (fs_hal_init(argc, argv) != 0 || dev_motor_init() != 0)
    {
        fs_hal_log("initialisation failed");
        return 1;
    }
    fs_hal_check_power();

    fs_time_init(epoch);
    fs_node_init(FLATSAT_NODE_ADCS, &cbs);
    fs_can_subscribe(FLATSAT_CAN_RW_CMD_TYPE, on_rw_cmd);
    wheel_ctrl_init(&ctrl, KP, KI, RPM_PER_DUTY, SPEED_FILTER, INTEGRAL_BAND, DEV_MOTOR_COUNTS_PER_REV);
    stop_wheel();
    fs_sched_init(tasks, sizeof(tasks) / sizeof(tasks[0]));

    fs_hal_log("ADCS node started, reset cause %u", fs_hal_reset_cause());
    for (;;)
    {
        uint32_t wait_us;

        fs_can_poll();
        fs_node_service();
        wait_us = fs_sched_run();
        fs_hal_sleep_us(wait_us < 1000u ? wait_us : 1000u);
    }
}

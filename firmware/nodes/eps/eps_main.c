/*
** EPS node: measures the FlatSat's real power and controls its fault-injection switches (ICD 6)
**
** - samples the three INA219 rails at 10 Hz; EPS_RAIL x3, EPS_BATT and EPS_SW_TLM every second
** - EPS_SW_CMD (OBC): switch 0 = ADCS node RUN pin, 1 = wheel motor driver; answered with EPS_SW_TLM
** - overcurrent protection: ADCS rail above 500 mA for 3 consecutive samples opens the motor switch
**   and reports FAULT (the switch stays open until the OBC closes it)
** - switch states are kept if the OBC is lost (ICD 6.5)
** Linux (SIL): eps_node [--can vcan0]
*/
#include <string.h>

#include "dev_power.h"
#include "flatsat_icd.h"
#include "fs_can.h"
#include "fs_hal.h"
#include "fs_node.h"
#include "fs_persist.h"
#include "fs_sched.h"
#include "fs_time.h"

#define SAMPLE_PERIOD_MS 100
#define TLM_PERIOD_MS    1000
#define OC_LIMIT_MA      500
#define OC_PERSISTENCE   3

/* EPS_RAIL.FLAGS / EPS_SW_TLM.FAULT_MASK bits */
#define RAIL_FLAG_OVERCURRENT 0x01
#define RAIL_FLAG_READ_ERROR  0x02

/* FAULT.CODE values sent by this node */
#define FAULT_OVERCURRENT 1
#define FAULT_SENSOR      2

static uint16_t     rail_mv[DEV_POWER_NUM_RAILS];
static int16_t      rail_ma[DEV_POWER_NUM_RAILS];
static uint8_t      rail_flags[DEV_POWER_NUM_RAILS];
static uint8_t      fault_mask;
static uint8_t      sw_seq;
static fs_persist_t oc_filter;

static uint8_t switch_mask(void)
{
    uint8_t m = 0;
    int     i;

    for (i = 0; i < DEV_POWER_NUM_SWITCHES; i++)
    {
        m |= (uint8_t)(dev_power_get_switch((uint8_t)i) ? (1u << i) : 0);
    }
    return m;
}

static void send_switch_tlm(void)
{
    flatsat_can_eps_sw_tlm_t t;

    memset(&t, 0, sizeof(t));
    t.state_mask = switch_mask();
    t.fault_mask = fault_mask;
    t.seq_echo   = sw_seq;
    fs_can_send(FLATSAT_CAN_EPS_SW_TLM_TYPE, &t);
}

static void send_fault(uint8_t code, uint8_t detail, uint16_t value)
{
    flatsat_can_fault_t f;

    memset(&f, 0, sizeof(f));
    f.code   = code;
    f.detail = detail;
    f.value  = value;
    fs_can_send(FLATSAT_CAN_FAULT_TYPE, &f);
}

/* ---- Handlers ---- */

static void on_sw_cmd(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_eps_sw_cmd_t *c = payload;

    (void)dlc;
    if (src != FLATSAT_NODE_OBC)
    {
        return;
    }
    sw_seq = c->seq;
    if (c->switch_id < DEV_POWER_NUM_SWITCHES)
    {
        dev_power_set_switch(c->switch_id, c->state == FLATSAT_SWITCH_STATE_ON);
        if (c->state == FLATSAT_SWITCH_STATE_ON)
        {
            fault_mask &= (uint8_t)~(1u << c->switch_id); /* closing a tripped switch clears its fault */
        }
        fs_hal_log("switch %u %s (seq %u)", c->switch_id, c->state ? "ON" : "OFF", c->seq);
    }
    send_switch_tlm();
}

static void enter_safe(const char *reason)
{
    /* Switches keep their state when the OBC is lost (ICD 6.5): the EPS just reports */
    fs_hal_log("OBC safe request: %s; switches unchanged", reason);
}

static uint8_t node_state(void)
{
    return fault_mask ? FLATSAT_NODE_STATE_FAULT : FLATSAT_NODE_STATE_NOMINAL;
}

/* ---- Tasks ---- */

static void task_sample(void)
{
    int i;

    for (i = 0; i < DEV_POWER_NUM_RAILS; i++)
    {
        rail_flags[i] &= (uint8_t)~RAIL_FLAG_READ_ERROR;
        if (dev_power_read_rail((uint8_t)i, &rail_mv[i], &rail_ma[i]) != 0)
        {
            rail_flags[i] |= RAIL_FLAG_READ_ERROR;
        }
    }

    /* Overcurrent on the ADCS rail: trip the motor driver */
    if (fs_persist_update(&oc_filter, rail_ma[DEV_POWER_RAIL_ADCS] <= OC_LIMIT_MA) == FS_PERSIST_TRIPPED)
    {
        dev_power_set_switch(DEV_POWER_SW_MOTOR, 0);
        fault_mask |= 1u << DEV_POWER_SW_MOTOR;
        rail_flags[DEV_POWER_RAIL_ADCS] |= RAIL_FLAG_OVERCURRENT;
        send_fault(FAULT_OVERCURRENT, DEV_POWER_RAIL_ADCS, (uint16_t)rail_ma[DEV_POWER_RAIL_ADCS]);
        send_switch_tlm();
        fs_hal_log("overcurrent on the ADCS rail (%d mA): motor switch opened", rail_ma[DEV_POWER_RAIL_ADCS]);
    }
    else if (!oc_filter.faulted)
    {
        rail_flags[DEV_POWER_RAIL_ADCS] &= (uint8_t)~RAIL_FLAG_OVERCURRENT;
    }
}

static void task_telemetry(void)
{
    flatsat_can_eps_rail_t r;
    flatsat_can_eps_batt_t b;
    int                    i;

    for (i = 0; i < DEV_POWER_NUM_RAILS; i++)
    {
        memset(&r, 0, sizeof(r));
        r.rail    = (uint8_t)i;
        r.flags   = rail_flags[i];
        r.voltage = rail_mv[i];
        r.current = rail_ma[i];
        r.power   = (uint16_t)((uint32_t)rail_mv[i] * (uint32_t)(rail_ma[i] > 0 ? rail_ma[i] : 0) / 1000u);
        fs_can_send(FLATSAT_CAN_EPS_RAIL_TYPE, &r);
    }

    memset(&b, 0, sizeof(b));
    b.voltage      = rail_mv[DEV_POWER_RAIL_BATTERY];
    b.current      = rail_ma[DEV_POWER_RAIL_BATTERY];
    b.charge_state = dev_power_charge_state();
    fs_can_send(FLATSAT_CAN_EPS_BATT_TYPE, &b);

    send_switch_tlm();
}

static fs_task_t tasks[] = {
    {.name = "sample", .period_ms = SAMPLE_PERIOD_MS, .offset_ms = 0, .fn = task_sample},
    {.name = "telemetry", .period_ms = TLM_PERIOD_MS, .offset_ms = 50, .fn = task_telemetry},
};

int main(int argc, char **argv)
{
    fs_node_callbacks_t cbs   = {.enter_safe = enter_safe, .state = node_state};
    fs_time_t           epoch = {814254200u, 0};

    if (fs_hal_init(argc, argv) != 0 || dev_power_init() != 0)
    {
        fs_hal_log("initialisation failed");
        return 1;
    }
    fs_time_init(epoch);
    fs_node_init(FLATSAT_NODE_EPS, &cbs);
    fs_can_subscribe(FLATSAT_CAN_EPS_SW_CMD_TYPE, on_sw_cmd);
    fs_persist_init(&oc_filter, OC_PERSISTENCE);
    fs_sched_init(tasks, sizeof(tasks) / sizeof(tasks[0]));

    fs_hal_log("EPS node started, reset cause %u, switches 0x%02x", fs_hal_reset_cause(), switch_mask());
    for (;;)
    {
        uint32_t wait_us;

        fs_can_poll();
        fs_node_service();
        wait_us = fs_sched_run();
        fs_hal_sleep_us(wait_us < 1000u ? wait_us : 1000u);
    }
}

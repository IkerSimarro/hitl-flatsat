/*
** OBC side of the FlatSat CAN bus (ICD 6)
**
** - broadcasts HEARTBEAT, TIME_SYNC and MODE every second (MODE also on change)
** - monitors the nodes' heartbeats: lost after 3 s, back, or rebooted (uptime went backwards); losing the
**   ADCS node in an actuating mode puts the system in SAFE (ICD 6.5)
** - commands the physical wheel at 10 Hz: it mirrors simulated wheel 0 at MIRROR_SCALE (ICD DD-05), or
**   follows the TEST-mode override; no RW_TLM for 500 ms while commanding is a wheel fault
** - collects the EPS node's measurements for EPS_REAL and forwards switch and reset commands
*/
#include <math.h>
#include <string.h>

#include "adcs_params.h"
#include "fs_can.h"
#include "fs_hal.h"
#include "fs_time.h"
#include "obc.h"

#define NODE_TIMEOUT_US     3000000u
#define RW_TLM_TIMEOUT_US   500000u
#define MIRROR_SCALE        0.1     /* physical rpm per simulated rpm (~6000 rpm sim wheel, ~600 rpm N20) */
#define MIRROR_MAX_RPM      550     /* stay inside the N20's speed on battery voltage */

static const uint8_t monitored[] = {FLATSAT_NODE_ADCS, FLATSAT_NODE_EPS};

static uint8_t  last_mode_sent = 0xFF;
static uint8_t  rw_seq;
static uint8_t  sw_seq;
static uint64_t rw_cmd_since_us; /* when the wheel was last commanded from OFF to on, 0 = not commanding */

static const char *node_name(uint8_t node)
{
    return node == FLATSAT_NODE_ADCS ? "ADCS" : node == FLATSAT_NODE_EPS ? "EPS" : "unknown";
}

/* ---- Receive handlers ---- */

static void on_heartbeat(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_heartbeat_t *hb = payload;
    obc_node_t                    *n;
    uint64_t                       now;

    (void)dlc;
    if (src >= OBC_MAX_NODES)
    {
        return;
    }
    n   = &obc.nodes[src];
    now = fs_hal_time_us();
    /* Heartbeats are 1 s apart, so a node that kept running reports a higher uptime each time. The same uptime
       more than half a period later means it restarted within its first second (a duplicated CAN frame, which a
       late error can cause, arrives straight away) */
    if (n->alive && (hb->uptime < n->uptime || (hb->uptime == n->uptime && now - n->last_us > 500000u)))
    {
        obc_event(EVT_NODE_REBOOTED, FLATSAT_SEVERITY_WARNING, "%s node rebooted (reset cause %u)", node_name(src),
                  hb->reset_cause);
    }
    if (!n->alive)
    {
        obc_event(EVT_NODE_UP, FLATSAT_SEVERITY_INFO, "%s node up (uptime %u s, reset cause %u)", node_name(src),
                  hb->uptime, hb->reset_cause);
    }
    n->alive       = 1;
    n->last_us     = now;
    n->uptime      = hb->uptime;
    n->state       = hb->state;
    n->reset_cause = hb->reset_cause;
}

static void on_fault(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_fault_t *f = payload;

    (void)dlc;
    obc_event(EVT_NODE_FAULT, FLATSAT_SEVERITY_ERROR, "%s node fault code %u detail %u value %u", node_name(src),
              f->code, f->detail, f->value);
}

static void on_rw_tlm(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_rw_tlm_t *t = payload;

    (void)dlc;
    if (src != FLATSAT_NODE_ADCS || t->wheel != 0)
    {
        return;
    }
    obc.phys_wheel.meas_rpm    = t->speed;
    obc.phys_wheel.status      = t->status;
    obc.phys_wheel.last_tlm_us = fs_hal_time_us();
    if (obc.phys_wheel.tlm_fault)
    {
        obc.phys_wheel.tlm_fault = 0;
        obc_event(EVT_WHEEL_FAULT, FLATSAT_SEVERITY_INFO, "physical wheel telemetry back");
    }
}

static void on_eps_rail(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_eps_rail_t *r = payload;

    (void)dlc;
    if (src == FLATSAT_NODE_EPS && r->rail < 3)
    {
        obc.eps_real.rail_mv[r->rail]    = r->voltage;
        obc.eps_real.rail_ma[r->rail]    = r->current;
        obc.eps_real.rail_mw[r->rail]    = r->power;
        obc.eps_real.rail_flags[r->rail] = r->flags;
    }
}

static void on_eps_batt(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_eps_batt_t *b = payload;

    (void)dlc;
    if (src == FLATSAT_NODE_EPS)
    {
        obc.eps_real.batt_mv      = b->voltage;
        obc.eps_real.batt_ma      = b->current;
        obc.eps_real.charge_state = b->charge_state;
    }
}

static void on_eps_sw_tlm(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_eps_sw_tlm_t *t = payload;

    (void)dlc;
    if (src == FLATSAT_NODE_EPS)
    {
        obc.eps_real.switch_mask       = t->state_mask;
        obc.eps_real.switch_fault_mask = t->fault_mask;
    }
}

static void on_node_ack(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_node_ack_t *a = payload;

    (void)dlc;
    obc_event(EVT_NODE_ACK, FLATSAT_SEVERITY_INFO, "%s node acknowledged command %u (result %u)", node_name(src),
              a->cmd, a->result);
}

/* ---- Periodic ---- */

static void send_mode(void)
{
    flatsat_can_mode_t m;

    memset(&m, 0, sizeof(m));
    m.mode = obc.mode;
    fs_can_send(FLATSAT_CAN_MODE_TYPE, &m);
    last_mode_sent = obc.mode;
}

/* Physical wheel set point (rpm): the TEST override, or simulated wheel 0 mirrored at MIRROR_SCALE */
static void wheel_command(uint8_t *ctrl_mode, int16_t *setpoint)
{
    double sim_rpm;
    double rpm;

    if (obc.mode == FLATSAT_MODE_TEST && obc.phys_wheel.test_override)
    {
        *ctrl_mode = obc.phys_wheel.test_mode;
        *setpoint  = obc.phys_wheel.test_setpoint;
        return;
    }
    if (obc.mode == FLATSAT_MODE_SAFE || obc.mode == FLATSAT_MODE_LOW_POWER || !(obc.sensor_valid & OBC_VALID_RW))
    {
        *ctrl_mode = FLATSAT_RW_CTRL_MODE_OFF;
        *setpoint  = 0;
        return;
    }
    sim_rpm = obc.rw_momentum[0] / ADCS_RW_INERTIA * 60.0 / (2.0 * M_PI);
    rpm     = sim_rpm * MIRROR_SCALE;
    rpm     = rpm > MIRROR_MAX_RPM ? MIRROR_MAX_RPM : (rpm < -MIRROR_MAX_RPM ? -MIRROR_MAX_RPM : rpm);
    *ctrl_mode = FLATSAT_RW_CTRL_MODE_SPEED;
    *setpoint  = (int16_t)lround(rpm);
}

void obc_can_task_fast(void)
{
    flatsat_can_rw_cmd_t c;
    uint8_t              ctrl_mode;
    int16_t              setpoint;
    uint64_t             now = fs_hal_time_us();

    if (obc.mode != last_mode_sent)
    {
        send_mode();
    }

    /* Locals, not &c.field: the message struct is packed, so its members may be misaligned */
    wheel_command(&ctrl_mode, &setpoint);
    memset(&c, 0, sizeof(c));
    c.wheel     = 0;
    c.ctrl_mode = ctrl_mode;
    c.setpoint  = setpoint;
    c.seq       = rw_seq++;
    fs_can_send(FLATSAT_CAN_RW_CMD_TYPE, &c);
    obc.phys_wheel.cmd_mode = c.ctrl_mode;
    obc.phys_wheel.cmd_rpm  = c.ctrl_mode == FLATSAT_RW_CTRL_MODE_SPEED ? c.setpoint : 0;

    /* Wheel telemetry watchdog while the wheel is commanded (ICD 6.5) */
    if (c.ctrl_mode == FLATSAT_RW_CTRL_MODE_OFF)
    {
        rw_cmd_since_us = 0;
    }
    else if (rw_cmd_since_us == 0)
    {
        rw_cmd_since_us = now;
    }
    if (rw_cmd_since_us != 0 && now - rw_cmd_since_us > RW_TLM_TIMEOUT_US && !obc.phys_wheel.tlm_fault &&
        now - obc.phys_wheel.last_tlm_us > RW_TLM_TIMEOUT_US)
    {
        obc.phys_wheel.tlm_fault = 1;
        obc_event(EVT_WHEEL_FAULT, FLATSAT_SEVERITY_ERROR, "no physical wheel telemetry for %u ms",
                  RW_TLM_TIMEOUT_US / 1000u);
        if (obc.mode == FLATSAT_MODE_DETUMBLE || obc.mode == FLATSAT_MODE_SUN_POINT)
        {
            obc_mode_request(FLATSAT_MODE_SAFE, FLATSAT_MODE_REASON_FAULT);
        }
    }
}

void obc_can_task_slow(void)
{
    flatsat_can_heartbeat_t hb;
    flatsat_can_time_sync_t ts;
    fs_time_t               now = fs_time_now();
    uint64_t                now_us = fs_hal_time_us();
    unsigned                i;

    memset(&hb, 0, sizeof(hb));
    hb.uptime      = (uint32_t)(now_us / 1000000u);
    hb.state       = obc.mode == FLATSAT_MODE_SAFE ? FLATSAT_NODE_STATE_SAFE : FLATSAT_NODE_STATE_NOMINAL;
    hb.reset_cause = obc.reset_cause;
    fs_hal_can_error_counters(&hb.tec, &hb.rec);
    fs_can_send(FLATSAT_CAN_HEARTBEAT_TYPE, &hb);

    memset(&ts, 0, sizeof(ts));
    ts.seconds    = now.seconds;
    ts.subseconds = now.subseconds;
    fs_can_send(FLATSAT_CAN_TIME_SYNC_TYPE, &ts);
    send_mode();

    for (i = 0; i < sizeof(monitored); i++)
    {
        obc_node_t *n = &obc.nodes[monitored[i]];
        if (n->alive && now_us - n->last_us > NODE_TIMEOUT_US)
        {
            n->alive = 0;
            obc_event(EVT_NODE_LOST, FLATSAT_SEVERITY_ERROR, "%s node lost (no heartbeat for %u s)",
                      node_name(monitored[i]), NODE_TIMEOUT_US / 1000000u);
            if (monitored[i] == FLATSAT_NODE_ADCS &&
                (obc.mode == FLATSAT_MODE_DETUMBLE || obc.mode == FLATSAT_MODE_SUN_POINT))
            {
                obc_mode_request(FLATSAT_MODE_SAFE, FLATSAT_MODE_REASON_NODE_LOST);
            }
        }
    }
}

/* ---- Interface ---- */

void obc_can_init(void)
{
    fs_can_init(FLATSAT_NODE_OBC);
    fs_can_subscribe(FLATSAT_CAN_HEARTBEAT_TYPE, on_heartbeat);
    fs_can_subscribe(FLATSAT_CAN_FAULT_TYPE, on_fault);
    fs_can_subscribe(FLATSAT_CAN_RW_TLM_TYPE, on_rw_tlm);
    fs_can_subscribe(FLATSAT_CAN_EPS_RAIL_TYPE, on_eps_rail);
    fs_can_subscribe(FLATSAT_CAN_EPS_BATT_TYPE, on_eps_batt);
    fs_can_subscribe(FLATSAT_CAN_EPS_SW_TLM_TYPE, on_eps_sw_tlm);
    fs_can_subscribe(FLATSAT_CAN_NODE_ACK_TYPE, on_node_ack);
}

void obc_can_poll(void)
{
    fs_can_poll();
}

uint8_t obc_can_alive_mask(void)
{
    uint8_t  m = 1u << FLATSAT_NODE_OBC;
    unsigned i;

    for (i = 0; i < sizeof(monitored); i++)
    {
        if (obc.nodes[monitored[i]].alive)
        {
            m |= (uint8_t)(1u << monitored[i]);
        }
    }
    return m;
}

int obc_can_switch(uint8_t sw, uint8_t on)
{
    flatsat_can_eps_sw_cmd_t c;

    if (sw > 1)
    {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.switch_id = sw;
    c.state     = on ? FLATSAT_SWITCH_STATE_ON : FLATSAT_SWITCH_STATE_OFF;
    c.seq       = ++sw_seq;
    return fs_can_send(FLATSAT_CAN_EPS_SW_CMD_TYPE, &c);
}

int obc_can_node_command(uint8_t node, uint8_t cmd)
{
    flatsat_can_node_cmd_t c;

    if (node != FLATSAT_NODE_ADCS && node != FLATSAT_NODE_EPS)
    {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.target = node;
    c.cmd    = cmd;
    return fs_can_send(FLATSAT_CAN_NODE_CMD_TYPE, &c);
}

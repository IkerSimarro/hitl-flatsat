/*
** Command handling (ICD 3.2 and 5)
**
** Packets are queued by the umbilical receive handler and executed from the main loop. Each command is
** checked for a valid CCSDS command header and checksum (fs_cmd_parse), a known MID and function code,
** and the exact length from the ICD, before it runs. Every rejection is counted and raises an event.
*/
#include <math.h>
#include <string.h>

#include "adcs_params.h"
#include "fs_ccsds.h"
#include "fs_hal.h"
#include "obc.h"

#define CMD_QUEUE_LEN 8
#define CMD_MAX_LEN   64
#define REBOOT_MAGIC  0x0B0075EDu
/* Wheel momentum limit over rotor inertia: ~629 rad/s */
#define RW_MANUAL_MAX_SPEED 600.0f

typedef struct
{
    uint8_t   data[CMD_MAX_LEN];
    size_t    len;
    fs_time_t rx_time;
    uint8_t   source; /* OBC_CMD_SRC_* */
} queued_cmd_t;

static queued_cmd_t queue[CMD_QUEUE_LEN];
static uint8_t      current_source = OBC_CMD_SRC_UMB;
static unsigned     q_head;
static unsigned     q_tail;

/* Expected total length of every command, from the generated ICD tables */
typedef struct
{
    uint16_t mid;
    uint8_t  fc;
    uint16_t len;
} cmd_def_t;

#define CMD_DEF(name, mid, fc, len) {mid, fc, len},
static const cmd_def_t cmd_defs[] = {FLATSAT_CMD_LIST(CMD_DEF)};
#undef CMD_DEF

void obc_cmd_init(void)
{
    q_head = q_tail = 0;
}

void obc_cmd_enqueue(const uint8_t *pkt, size_t len, uint8_t source)
{
    unsigned next = (q_head + 1) % CMD_QUEUE_LEN;

    if (next == q_tail || len > CMD_MAX_LEN)
    {
        /* Can't raise an event from here (it may need the bus); count it */
        obc.cmd_reject_count++;
        return;
    }
    memcpy(queue[q_head].data, pkt, len);
    queue[q_head].len     = len;
    queue[q_head].rx_time = fs_time_now();
    queue[q_head].source  = source;
    q_head                = next;
}

static void reject(uint16_t mid, uint8_t fc, const char *why)
{
    obc.cmd_reject_count++;
    obc_event(EVT_CMD_REJECTED, FLATSAT_SEVERITY_ERROR, "command MID 0x%04X FC %u rejected: %s", mid, fc, why);
}

static void accept(const fs_cmd_t *cmd)
{
    obc.cmd_accept_count++;
    obc.last_cmd_mid = cmd->mid;
    obc.last_cmd_fc  = cmd->fc;
}

/* Copies the arguments into an aligned struct (args in the packet are not aligned) */
#define ARGS(type, cmd, var) \
    type var;                \
    memcpy(&var, (cmd)->args, sizeof(var))

static void exec_obc(const fs_cmd_t *cmd, fs_time_t rx_time)
{
    switch (cmd->fc)
    {
        case FLATSAT_OBC_NOOP_FC:
            accept(cmd);
            obc_event(EVT_CMD_NOOP, FLATSAT_SEVERITY_INFO, "NOOP received, ICD %s", FLATSAT_ICD_VERSION);
            break;

        case FLATSAT_OBC_RESET_COUNTERS_FC:
            obc.cmd_accept_count = 0;
            obc.cmd_reject_count = 0;
            obc.event_count      = 0;
            accept(cmd);
            break;

        case FLATSAT_OBC_SET_MODE_FC:
        {
            ARGS(flatsat_obc_set_mode_t, cmd, a);
            if (obc_mode_request(a.mode, FLATSAT_MODE_REASON_COMMAND))
            {
                accept(cmd);
            }
            else
            {
                obc.cmd_reject_count++;
            }
            break;
        }

        case FLATSAT_OBC_SET_TIME_FC:
        {
            ARGS(flatsat_obc_set_time_t, cmd, a);
            fs_time_t t = {a.seconds, a.subseconds};
            fs_time_sync(t, 0);
            accept(cmd);
            obc_event(EVT_TIME_SET, FLATSAT_SEVERITY_INFO, "time set to %u s, correction %lld us", a.seconds,
                      (long long)fs_time_last_correction_us());
            break;
        }

        case FLATSAT_OBC_PING_FC:
        {
            ARGS(flatsat_obc_ping_t, cmd, a);
            accept(cmd);
            obc_tlm_send_ping_reply(a.token, rx_time, current_source);
            break;
        }

        case FLATSAT_OBC_SET_TLM_PERIOD_FC:
        {
            ARGS(flatsat_obc_set_tlm_period_t, cmd, a);
            if (obc_tlm_set_period(a.tlm_mid, a.period))
            {
                accept(cmd);
                obc_event(EVT_TLM_PERIOD, FLATSAT_SEVERITY_INFO, "MID 0x%04X period %u ms", a.tlm_mid, a.period);
            }
            else
            {
                reject(cmd->mid, cmd->fc, "not a periodic telemetry MID");
            }
            break;
        }

        case FLATSAT_OBC_DOWNLINK_PACKET_FC:
        {
            ARGS(flatsat_obc_downlink_packet_t, cmd, a);
            uint8_t pkt[FLATSAT_RF_MAX_PACKET];
            size_t  n = obc_tlm_build(a.tlm_mid, pkt, sizeof(pkt));
            if (n == 0 || !obc_comms_queue(pkt, n))
            {
                reject(cmd->mid, cmd->fc, "not a telemetry packet that fits an RF frame");
                break;
            }
            accept(cmd);
            break;
        }

        case FLATSAT_OBC_NODE_RESET_FC:
        {
            ARGS(flatsat_obc_node_reset_t, cmd, a);
            if (obc_can_node_command(a.node, FLATSAT_NODE_CMD_RESET) == 0)
            {
                accept(cmd);
            }
            else
            {
                reject(cmd->mid, cmd->fc, "not a resettable node");
            }
            break;
        }

        case FLATSAT_OBC_REBOOT_FC:
        {
            ARGS(flatsat_obc_reboot_t, cmd, a);
            if (a.magic != REBOOT_MAGIC)
            {
                reject(cmd->mid, cmd->fc, "wrong reboot magic");
                break;
            }
            accept(cmd);
            obc_event(EVT_REBOOT, FLATSAT_SEVERITY_WARNING, "reboot commanded");
            fs_hal_reboot();
            break;
        }

        case FLATSAT_OBC_SET_AUTO_MODES_FC:
        {
            ARGS(flatsat_obc_set_auto_modes_t, cmd, a);
            if (a.state > FLATSAT_SWITCH_STATE_ON)
            {
                reject(cmd->mid, cmd->fc, "state must be OFF or ON");
                break;
            }
            obc.adcs.auto_modes = a.state;
            accept(cmd);
            obc_event(EVT_ADCS, FLATSAT_SEVERITY_INFO, "automatic mode transitions %s", a.state ? "on" : "off");
            break;
        }

        default:
            reject(cmd->mid, cmd->fc, "unknown function code");
            break;
    }
}

static void exec_adcs(const fs_cmd_t *cmd)
{
    switch (cmd->fc)
    {
        case FLATSAT_ADCS_NOOP_FC:
            accept(cmd);
            obc_event(EVT_CMD_NOOP, FLATSAT_SEVERITY_INFO, "ADCS NOOP received");
            break;

        case FLATSAT_ADCS_SET_BDOT_GAIN_FC:
        {
            ARGS(flatsat_adcs_set_bdot_gain_t, cmd, a);
            if (!(a.gain > 0.0f && a.gain <= 10000.0f)) /* also rejects NaN */
            {
                reject(cmd->mid, cmd->fc, "B-dot gain out of range (0, 10000]");
                break;
            }
            obc.bdot_gain = a.gain;
            accept(cmd);
            break;
        }

        case FLATSAT_ADCS_SET_SUN_GAINS_FC:
        {
            ARGS(flatsat_adcs_set_sun_gains_t, cmd, a);
            if (!(a.kp > 0.0f && a.kp <= 1.0f && a.kd > 0.0f && a.kd <= 10.0f))
            {
                reject(cmd->mid, cmd->fc, "sun gains out of range: kp (0, 1], kd (0, 10]");
                break;
            }
            obc.sun_kp = a.kp;
            obc.sun_kd = a.kd;
            accept(cmd);
            break;
        }

        case FLATSAT_ADCS_RW_MANUAL_FC:
        {
            ARGS(flatsat_adcs_rw_manual_t, cmd, a);
            if (obc.mode != FLATSAT_MODE_TEST)
            {
                reject(cmd->mid, cmd->fc, "manual actuators only in TEST mode");
            }
            else if (a.wheel >= DEV_NUM_RW || !(fabsf(a.speed) <= RW_MANUAL_MAX_SPEED))
            {
                reject(cmd->mid, cmd->fc, "invalid wheel or speed");
            }
            else
            {
                obc.adcs.manual_rw[a.wheel]       = 1;
                obc.adcs.manual_rw_speed[a.wheel] = a.speed;
                accept(cmd);
            }
            break;
        }

        case FLATSAT_ADCS_TRQ_MANUAL_FC:
        {
            ARGS(flatsat_adcs_trq_manual_t, cmd, a);
            if (obc.mode != FLATSAT_MODE_TEST)
            {
                reject(cmd->mid, cmd->fc, "manual actuators only in TEST mode");
            }
            else if (a.torquer >= DEV_NUM_TRQ || dev_trq_set(a.torquer, a.duty / 10000.0f) != DEV_OK)
            {
                reject(cmd->mid, cmd->fc, "invalid torquer or duty");
            }
            else
            {
                obc.trq_duty[a.torquer] = a.duty;
                accept(cmd);
            }
            break;
        }

        case FLATSAT_ADCS_PHYS_WHEEL_TEST_FC:
        {
            ARGS(flatsat_adcs_phys_wheel_test_t, cmd, a);
            if (obc.mode != FLATSAT_MODE_TEST)
            {
                reject(cmd->mid, cmd->fc, "physical wheel test only in TEST mode");
            }
            else if (a.ctrl_mode > FLATSAT_RW_CTRL_MODE_DUTY ||
                     (a.ctrl_mode == FLATSAT_RW_CTRL_MODE_DUTY && (a.setpoint > 10000 || a.setpoint < -10000)))
            {
                reject(cmd->mid, cmd->fc, "invalid wheel mode or set point");
            }
            else
            {
                obc.phys_wheel.test_override = 1;
                obc.phys_wheel.test_mode     = a.ctrl_mode;
                obc.phys_wheel.test_setpoint = a.setpoint;
                accept(cmd);
            }
            break;
        }

        default:
            reject(cmd->mid, cmd->fc, "unknown function code");
            break;
    }
}

static void exec_eps(const fs_cmd_t *cmd)
{
    switch (cmd->fc)
    {
        case FLATSAT_EPS_NOOP_FC:
            accept(cmd);
            obc_event(EVT_CMD_NOOP, FLATSAT_SEVERITY_INFO, "EPS NOOP received");
            break;

        case FLATSAT_EPS_SWITCH_FC:
        {
            ARGS(flatsat_eps_switch_t, cmd, a);
            if (obc_can_switch(a.switch_id, a.state) == 0)
            {
                accept(cmd);
                obc_event(EVT_EPS_SWITCH, FLATSAT_SEVERITY_INFO, "FlatSat switch %u %s requested", a.switch_id,
                          a.state ? "ON" : "OFF");
            }
            else
            {
                reject(cmd->mid, cmd->fc, "invalid switch");
            }
            break;
        }

        case FLATSAT_EPS_SIM_SWITCH_FC:
        {
            ARGS(flatsat_eps_sim_switch_t, cmd, a);
            int rc = dev_eps_set_switch(a.switch_id, a.state);
            if (rc == DEV_OK)
            {
                accept(cmd);
                obc_event(EVT_EPS_SWITCH, FLATSAT_SEVERITY_INFO, "simulated EPS switch %u %s", a.switch_id,
                          a.state ? "ON" : "OFF");
            }
            else
            {
                reject(cmd->mid, cmd->fc, rc == DEV_ERR_ARG ? "invalid switch" : "EPS did not confirm");
            }
            break;
        }

        default:
            reject(cmd->mid, cmd->fc, "unknown function code");
            break;
    }
}

static void exec_comms(const fs_cmd_t *cmd)
{
    switch (cmd->fc)
    {
        case FLATSAT_COMMS_NOOP_FC:
            accept(cmd);
            obc_event(EVT_CMD_NOOP, FLATSAT_SEVERITY_INFO, "COMMS NOOP received via %s",
                      current_source == OBC_CMD_SRC_RF ? "RF" : "umbilical");
            break;

        case FLATSAT_COMMS_SET_TX_POWER_FC:
        {
            ARGS(flatsat_comms_set_tx_power_t, cmd, a);
            if (!obc_comms_set_tx_power(a.power))
            {
                reject(cmd->mid, cmd->fc, "TX power outside -9..22 dBm");
                break;
            }
            accept(cmd);
            break;
        }

        case FLATSAT_COMMS_SET_BEACON_PERIOD_FC:
        {
            ARGS(flatsat_comms_set_beacon_period_t, cmd, a);
            if (!obc_comms_set_beacon_period(a.period))
            {
                reject(cmd->mid, cmd->fc, "beacon period must be 0 (off) or 5..3600 s");
                break;
            }
            accept(cmd);
            break;
        }

        default:
            reject(cmd->mid, cmd->fc, "unknown function code");
            break;
    }
}

static void execute(const uint8_t *pkt, size_t len, fs_time_t rx_time)
{
    fs_cmd_t        cmd;
    fs_cmd_status_t st = fs_cmd_parse(pkt, len, &cmd);
    size_t          i;
    int             known = 0;

    if (st != FS_CMD_OK)
    {
        static const char *why[] = {"ok", "too short", "not a command", "length field mismatch", "bad checksum"};
        reject(fs_pkt_mid(pkt, len), len > 6 ? pkt[6] : 0, why[st]);
        return;
    }

    for (i = 0; i < sizeof(cmd_defs) / sizeof(cmd_defs[0]); i++)
    {
        if (cmd_defs[i].mid == cmd.mid && cmd_defs[i].fc == cmd.fc)
        {
            known = 1;
            if (cmd_defs[i].len != len)
            {
                reject(cmd.mid, cmd.fc, "wrong length for this command");
                return;
            }
            break;
        }
    }
    if (!known)
    {
        reject(cmd.mid, cmd.fc, "unknown command");
        return;
    }

    switch (cmd.mid)
    {
        case FLATSAT_OBC_CMD_MID:
            exec_obc(&cmd, rx_time);
            break;
        case FLATSAT_ADCS_CMD_MID:
            exec_adcs(&cmd);
            break;
        case FLATSAT_EPS_CMD_MID:
            exec_eps(&cmd);
            break;
        case FLATSAT_COMMS_CMD_MID:
            exec_comms(&cmd);
            break;
        default:
            reject(cmd.mid, cmd.fc, "unknown MID");
            break;
    }
}

void obc_cmd_process(void)
{
    while (q_tail != q_head)
    {
        /* Copy out before freeing the slot: executing a command services the umbilical, which can queue more */
        queued_cmd_t c = queue[q_tail];
        q_tail         = (q_tail + 1) % CMD_QUEUE_LEN;
        current_source = c.source;
        execute(c.data, c.len, c.rx_time);
        current_source = OBC_CMD_SRC_UMB;
    }
}

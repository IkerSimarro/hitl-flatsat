/*
** Command handling (ICD 3.2 and 5)
**
** Packets are queued by the umbilical receive handler and executed from the main loop. Each command is
** checked for a valid CCSDS command header and checksum (fs_cmd_parse), a known MID and function code,
** and the exact length from the ICD, before it runs. Every rejection is counted and raises an event.
*/
#include <string.h>

#include "fs_ccsds.h"
#include "fs_hal.h"
#include "obc.h"

#define CMD_QUEUE_LEN 8
#define CMD_MAX_LEN   64
#define REBOOT_MAGIC  0x0B0075EDu

typedef struct
{
    uint8_t   data[CMD_MAX_LEN];
    size_t    len;
    fs_time_t rx_time;
} queued_cmd_t;

static queued_cmd_t queue[CMD_QUEUE_LEN];
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

void obc_cmd_enqueue(const uint8_t *pkt, size_t len)
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
    q_head                = next;
}

static void reject(uint16_t mid, uint8_t fc, const char *why)
{
    obc.cmd_reject_count++;
    obc_event(EVT_CMD_REJECTED, FLATSAT_SEVERITY_ERROR, "command MID 0x%04X FC %u rejected: %s", mid, fc, why);
}

static void not_implemented(const char *name)
{
    obc.cmd_reject_count++;
    obc_event(EVT_CMD_NOT_IMPLEMENTED, FLATSAT_SEVERITY_WARNING, "%s not implemented yet", name);
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
            obc_tlm_send_ping_reply(a.token, rx_time);
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
            not_implemented("OBC_DOWNLINK_PACKET (RF link, Phase 2)");
            break;

        case FLATSAT_OBC_NODE_RESET_FC:
            not_implemented("OBC_NODE_RESET (CAN nodes, Phase 2)");
            break;

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
            obc.bdot_gain = a.gain;
            accept(cmd);
            break;
        }

        case FLATSAT_ADCS_SET_SUN_GAINS_FC:
        {
            ARGS(flatsat_adcs_set_sun_gains_t, cmd, a);
            obc.sun_kp = a.kp;
            obc.sun_kd = a.kd;
            accept(cmd);
            break;
        }

        case FLATSAT_ADCS_RW_MANUAL_FC:
            not_implemented("ADCS_RW_MANUAL (wheel speed loop, Phase 2)");
            break;

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
            not_implemented("EPS_SWITCH (EPS node over CAN, Phase 2)");
            break;

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
    if (cmd->fc == FLATSAT_COMMS_NOOP_FC)
    {
        accept(cmd);
        obc_event(EVT_CMD_NOOP, FLATSAT_SEVERITY_INFO, "COMMS NOOP received");
    }
    else
    {
        not_implemented("COMMS settings (RF link, Phase 2)");
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
        execute(c.data, c.len, c.rx_time);
    }
}

/*
** Telemetry (ICD 4): builds each packet from the shared state and sends it at its configured rate
**
** Periodic packets go to the umbilical. The RF downlink (beacon, on-request packets) is added with the
** comms module in Phase 2; obc_tlm_send_packet() is the single place both paths will hang off.
*/
#include <math.h>
#include <string.h>

#include "fs_ccsds.h"
#include "fs_hal.h"
#include "fs_sched.h"
#include "fs_umbilical.h"
#include "obc.h"

typedef void (*tlm_builder_t)(void *payload);

typedef struct
{
    uint16_t      mid;
    uint16_t      payload_len;
    tlm_builder_t build;
    uint16_t      period_ms; /* 0 = off */
    uint64_t      next_us;
} tlm_entry_t;

static uint64_t boot_us;

static uint32_t uptime_s(void)
{
    return (uint32_t)((fs_hal_time_us() - boot_us) / 1000000u);
}

/* ---- Builders ---- */

static void build_obc_hk(void *p)
{
    flatsat_obc_hk_t *t = p;

    t->uptime           = uptime_s();
    t->cmd_accept_count = obc.cmd_accept_count;
    t->cmd_reject_count = obc.cmd_reject_count;
    t->last_cmd_mid     = obc.last_cmd_mid;
    t->last_cmd_fc      = obc.last_cmd_fc;
    t->mode             = obc.mode;
    t->mode_reason      = obc.mode_reason;
    t->reset_cause      = obc.reset_cause;
    t->reset_count      = obc.reset_count;
    t->cpu_load         = fs_sched_cpu_load();
    t->node_alive_mask  = 1u << FLATSAT_NODE_OBC; /* the other nodes join over CAN in Phase 2 */
    t->time_source      = fs_time_source();
    t->link_flags       = fs_umb_link_up() ? 0x01 : 0x00;
    fs_hal_can_error_counters(&t->can_tec, &t->can_rec);
    t->event_count = obc.event_count;
}

static void build_beacon(void *p)
{
    flatsat_beacon_t *t = p;
    double            w = 0;
    int               i;

    for (i = 0; i < 3; i++)
    {
        w += (double)obc.imu.rate[i] * obc.imu.rate[i];
    }
    t->uptime           = uptime_s();
    t->mode             = obc.mode;
    t->fault_flags      = (uint8_t)((~obc.sensor_valid) & 0xFF);
    t->cmd_accept_count = obc.cmd_accept_count;
    t->sim_batt_mv      = (uint16_t)(obc.eps.batt_v * 1000.0f);
    t->real_batt_mv     = 0; /* EPS node, Phase 2 */
    t->body_rate        = (uint16_t)(sqrt(w) * 1000.0);
    t->eclipse          = obc.eclipse;
    t->node_alive_mask  = 1u << FLATSAT_NODE_OBC;
    t->event_count      = obc.event_count;
}

static void build_adcs_state(void *p)
{
    flatsat_adcs_state_t *t = p;
    int                   i;

    t->adcs_mode = FLATSAT_ADCS_MODE_OFF; /* control laws arrive in Phase 2 */
    for (i = 0; i < 3; i++)
    {
        t->rate_est[i]     = obc.imu.rate[i];
        t->mag_body[i]     = obc.mag.field[i];
        t->rw_sim_speed[i] = (float)obc.rw_momentum[i]; /* momentum until the wheel inertia is in the ADCS module */
        t->trq_duty[i]     = obc.trq_duty[i];
    }
}

static void build_adcs_sensors(void *p)
{
    flatsat_adcs_sensors_t *t = p;
    int                     i;

    t->valid_mask = obc.sensor_valid & 0x3F;
    for (i = 0; i < 3; i++)
    {
        t->imu_rate[i]     = obc.imu.rate[i];
        t->imu_accel[i]    = obc.imu.accel[i];
        t->mag[i]          = obc.mag.field[i];
        t->gps_pos_ecef[i] = (float)obc.gps.pos[i];
        t->gps_vel_ecef[i] = (float)obc.gps.vel[i];
    }
    t->fss_alpha = obc.fss.alpha;
    t->fss_beta  = obc.fss.beta;
    for (i = 0; i < DEV_NUM_CSS; i++)
    {
        t->css[i] = obc.css.illum[i];
    }
    for (i = 0; i < 4; i++)
    {
        t->st_quat[i] = obc.st.q[i];
    }
    t->gps_week = obc.gps.week;
    t->gps_sow  = (float)obc.gps.seconds_of_week;
}

static void build_eps_sim(void *p)
{
    flatsat_eps_sim_t *t = p;

    t->batt_v      = obc.eps.batt_v;
    t->batt_temp   = obc.eps.batt_temp_c;
    t->bus_3v3     = obc.eps.bus_3v3_v;
    t->bus_5v0     = obc.eps.bus_5v0_v;
    t->bus_12v     = obc.eps.bus_12v_v;
    t->eps_temp    = obc.eps.eps_temp_c;
    t->sa_v        = obc.eps.sa_v;
    t->sa_temp     = obc.eps.sa_temp_c;
    t->switch_mask = obc.eps.switch_mask;
    t->eclipse     = obc.eclipse;
}

static void build_comms_stats(void *p)
{
    flatsat_comms_stats_t *t = p;
    const fs_umb_stats_t  *u = fs_umb_stats();

    t->umb_tx_packets = u->tx_frames;
    t->umb_rx_packets = u->rx_frames;
}

/* Default umbilical rates (ICD 4) */
static tlm_entry_t table[] = {
    {FLATSAT_OBC_HK_MID, sizeof(flatsat_obc_hk_t), build_obc_hk, 1000, 0},
    {FLATSAT_BEACON_MID, sizeof(flatsat_beacon_t), build_beacon, 10000, 0},
    {FLATSAT_ADCS_STATE_MID, sizeof(flatsat_adcs_state_t), build_adcs_state, 1000, 0},
    {FLATSAT_ADCS_SENSORS_MID, sizeof(flatsat_adcs_sensors_t), build_adcs_sensors, 1000, 0},
    {FLATSAT_EPS_SIM_MID, sizeof(flatsat_eps_sim_t), build_eps_sim, 1000, 0},
    {FLATSAT_COMMS_STATS_MID, sizeof(flatsat_comms_stats_t), build_comms_stats, 1000, 0},
};

#define TABLE_LEN (sizeof(table) / sizeof(table[0]))

/* Largest periodic payload, for the build buffer */
#define MAX_PAYLOAD sizeof(flatsat_adcs_sensors_t)

static void send_entry(tlm_entry_t *e)
{
    uint8_t   payload[MAX_PAYLOAD];
    uint8_t   pkt[FLATSAT_TLM_HDR_LEN + MAX_PAYLOAD];
    fs_time_t now = fs_time_now();
    size_t    n;

    memset(payload, 0, sizeof(payload));
    e->build(payload);
    n = fs_tlm_build(pkt, sizeof(pkt), e->mid, now.seconds, now.subseconds, payload, e->payload_len);
    if (n > 0)
    {
        obc_tlm_send_packet(pkt, n);
    }
}

void obc_tlm_init(void)
{
    unsigned i;

    boot_us = fs_hal_time_us();
    for (i = 0; i < TABLE_LEN; i++)
    {
        table[i].next_us = boot_us + (uint64_t)table[i].period_ms * 1000u;
    }
}

void obc_tlm_service(void)
{
    uint64_t now = fs_hal_time_us();
    unsigned i;

    for (i = 0; i < TABLE_LEN; i++)
    {
        tlm_entry_t *e = &table[i];
        if (e->period_ms != 0 && now >= e->next_us)
        {
            send_entry(e);
            e->next_us += (uint64_t)e->period_ms * 1000u;
            if (e->next_us <= now)
            {
                e->next_us = now + (uint64_t)e->period_ms * 1000u; /* fell behind: don't burst */
            }
        }
    }
}

int obc_tlm_set_period(uint16_t mid, uint16_t period_ms)
{
    unsigned i;

    for (i = 0; i < TABLE_LEN; i++)
    {
        if (table[i].mid == mid)
        {
            table[i].period_ms = period_ms;
            table[i].next_us   = fs_hal_time_us() + (uint64_t)period_ms * 1000u;
            return 1;
        }
    }
    return 0;
}

void obc_tlm_send_now(uint16_t mid)
{
    unsigned i;

    for (i = 0; i < TABLE_LEN; i++)
    {
        if (table[i].mid == mid)
        {
            send_entry(&table[i]);
        }
    }
}

void obc_tlm_send_ping_reply(uint32_t token, fs_time_t rx_time)
{
    flatsat_ping_reply_t r;
    uint8_t              pkt[FLATSAT_PING_REPLY_LEN];
    fs_time_t            now = fs_time_now();
    size_t               n;

    memset(&r, 0, sizeof(r));
    r.token         = token;
    r.rx_seconds    = rx_time.seconds;
    r.rx_subseconds = rx_time.subseconds;
    n = fs_tlm_build(pkt, sizeof(pkt), FLATSAT_PING_REPLY_MID, now.seconds, now.subseconds, &r, sizeof(r));
    if (n > 0)
    {
        obc_tlm_send_packet(pkt, n);
    }
}

void obc_tlm_send_packet(const uint8_t *pkt, size_t len)
{
    if (fs_umb_link_up())
    {
        fs_umb_send_tm(pkt, len);
    }
}

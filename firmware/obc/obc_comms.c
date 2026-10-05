/*
** RF link (ICD 7): beacon, store-and-forward downlink, airtime budget, telecommand reception
**
** - BEACON every beacon period (default 10 s) whatever the contact state, so the ground can find the spacecraft
** - in contact (a valid TC or HAIL frame received in the last 45 s, so one lost 20 s hail is tolerated): queued
**   packets go down oldest first. Events,
**   OBC_DOWNLINK_PACKET and replies to RF commands are queued; outside contact they wait
** - airtime over a rolling hour: other frames stop at the 5 % design budget (180 s), beacons at the legal 10 %
** - half duplex: one frame at a time, the radio is busy for the frame's time on air
** - LOW_POWER: beacons only, the queue waits
*/
#include <string.h>

#include "fs_hal.h"
#include "fs_rf.h"
#include "obc.h"

#define QUEUE_LEN          16
#define CONTACT_TIMEOUT_US 45000000u
#define DATA_BUDGET_MS     180000u /* 5 % of an hour */
#define BEACON_BUDGET_MS   360000u /* 10 %: the legal limit (ICD 7.1) */
#define DEFAULT_BEACON_S   10u
#define DEFAULT_TX_POWER   2       /* dBm: the boards share a desk */
#define MINUTE_US          60000000u

typedef struct
{
    uint8_t data[FS_RF_MAX_DATA];
    uint8_t len;
} queued_t;

static queued_t queue[QUEUE_LEN];
static unsigned q_first;
static unsigned q_count;

static uint32_t airtime_ms[60]; /* per minute, the last hour */
static uint64_t current_minute;
static uint64_t radio_busy_until_us;
static uint64_t last_heard_us;  /* 0 = never */
static uint64_t next_beacon_us;
static uint16_t tx_counter;
static int      contact_reported;

/* ---- Airtime accounting ---- */

static void roll_minutes(uint64_t now)
{
    uint64_t minute = now / MINUTE_US;

    while (current_minute < minute)
    {
        current_minute++;
        airtime_ms[current_minute % 60] = 0;
    }
}

static uint32_t airtime_last_hour_ms(void)
{
    uint32_t sum = 0;
    unsigned i;

    for (i = 0; i < 60; i++)
    {
        sum += airtime_ms[i];
    }
    return sum;
}

/* ---- Transmission ---- */

/* Sends one frame if the radio is free and the budget allows it; returns 1 if sent */
static int transmit(uint8_t type, const uint8_t *data, size_t len, uint32_t budget_ms)
{
    uint8_t  frame[FS_RF_MAX_FRAME];
    uint64_t now = fs_hal_time_us();
    uint32_t airtime_us;
    size_t   n;

    if (now < radio_busy_until_us)
    {
        return 0;
    }
    n = fs_rf_build(frame, sizeof(frame), type, tx_counter, data, len);
    if (n == 0)
    {
        return 0;
    }
    airtime_us = fs_rf_airtime_us(n);
    if (airtime_last_hour_ms() + (airtime_us + 999u) / 1000u > budget_ms)
    {
        return 0; /* deferred: the budget frees up as old minutes roll out of the hour */
    }
    if (dev_radio_send(frame, n) != DEV_OK)
    {
        return 0;
    }
    tx_counter++;
    radio_busy_until_us = now + airtime_us;
    airtime_ms[current_minute % 60] += (airtime_us + 999u) / 1000u;
    obc.comms.tx_frames++;
    return 1;
}

static void send_beacon(void)
{
    uint8_t pkt[FS_RF_MAX_DATA];
    size_t  n = obc_tlm_build(FLATSAT_BEACON_MID, pkt, sizeof(pkt));

    if (n > 0 && transmit(FLATSAT_RF_FRAME_TYPE_BEACON, pkt, n, BEACON_BUDGET_MS))
    {
        next_beacon_us = fs_hal_time_us() + (uint64_t)obc.comms.beacon_period_s * 1000000u;
    }
}

static int in_contact(uint64_t now)
{
    return last_heard_us != 0 && now - last_heard_us < CONTACT_TIMEOUT_US;
}

/* ---- Interface ---- */

int obc_comms_queue(const uint8_t *pkt, size_t len)
{
    queued_t *slot;

    if (len == 0 || len > FS_RF_MAX_DATA)
    {
        return 0;
    }
    if (q_count == QUEUE_LEN)
    {
        /* Full: the oldest packet makes room (the newest state is worth more) */
        q_first = (q_first + 1) % QUEUE_LEN;
        q_count--;
        obc.comms.queue_drops++;
    }
    slot = &queue[(q_first + q_count) % QUEUE_LEN];
    memcpy(slot->data, pkt, len);
    slot->len = (uint8_t)len;
    q_count++;
    return 1;
}

/* Called from the umbilical receive handler: no bus access here */
void obc_comms_on_rx(const uint8_t *frame, size_t len, int16_t rssi, int8_t snr)
{
    fs_rf_frame_t f;
    int           rc = fs_rf_parse(frame, len, &f);

    if (rc == FS_RF_ERR_CRC)
    {
        obc.comms.crc_errors++;
        return;
    }
    if (rc != FS_RF_OK || (f.type != FLATSAT_RF_FRAME_TYPE_TC && f.type != FLATSAT_RF_FRAME_TYPE_HAIL))
    {
        obc.comms.rejected++;
        return;
    }
    obc.comms.rx_frames++;
    obc.comms.last_rssi = rssi;
    obc.comms.last_snr  = snr;
    last_heard_us       = fs_hal_time_us();
    if (f.type == FLATSAT_RF_FRAME_TYPE_TC)
    {
        obc_cmd_enqueue(f.data, f.data_len, OBC_CMD_SRC_RF);
    }
}

void obc_comms_task(void)
{
    uint64_t now = fs_hal_time_us();
    int      contact;

    roll_minutes(now);
    contact = in_contact(now);
    if (contact != contact_reported)
    {
        contact_reported = contact;
        obc_event(EVT_COMMS, FLATSAT_SEVERITY_INFO, contact ? "RF contact: ground station heard, %u packets queued"
                                                            : "RF contact lost (%u packets queued)",
                  q_count);
    }

    if (obc.comms.beacon_period_s != 0 && now >= next_beacon_us)
    {
        send_beacon();
    }
    else if (contact && q_count > 0 && obc.mode != FLATSAT_MODE_LOW_POWER)
    {
        queued_t *q = &queue[q_first];
        if (transmit(FLATSAT_RF_FRAME_TYPE_TM, q->data, q->len, DATA_BUDGET_MS))
        {
            q_first = (q_first + 1) % QUEUE_LEN;
            q_count--;
        }
    }

    obc.comms.contact     = (uint8_t)contact;
    obc.comms.queue_depth = (uint16_t)q_count;
    obc.comms.duty_permil = (uint16_t)(airtime_last_hour_ms() / 3600u); /* 0.1 % of an hour = 3600 ms */
}

int obc_comms_set_beacon_period(uint16_t seconds)
{
    if (seconds != 0 && (seconds < 5 || seconds > 3600))
    {
        return 0;
    }
    obc.comms.beacon_period_s = seconds;
    next_beacon_us            = fs_hal_time_us(); /* the next beacon goes out now, then at the new period */
    return 1;
}

int obc_comms_set_tx_power(int8_t dbm)
{
    if (dbm < -9 || dbm > 22 || dev_radio_set_power(dbm) != DEV_OK) /* SX1262 range */
    {
        return 0;
    }
    obc.comms.tx_power_dbm = dbm;
    return 1;
}

void obc_comms_init(void)
{
    memset(&obc.comms, 0, sizeof(obc.comms));
    memset(airtime_ms, 0, sizeof(airtime_ms));
    q_first = q_count = 0;
    current_minute      = fs_hal_time_us() / MINUTE_US;
    radio_busy_until_us = 0;
    last_heard_us       = 0;
    contact_reported    = 0;
    tx_counter          = 0;
    obc.comms.beacon_period_s = DEFAULT_BEACON_S;
    obc.comms.tx_power_dbm    = DEFAULT_TX_POWER;
    dev_radio_set_power(DEFAULT_TX_POWER);
    next_beacon_us = fs_hal_time_us() + 2000000u; /* first beacon once the link has settled */
}

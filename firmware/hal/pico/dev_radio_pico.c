/*
** Radio (ICD 7.1): the Pico build
**
** The SX1262 on the Pico-LoRa-SX1262 board (radio_pico.c) when it answers at start-up. Without it, frames go over
** the umbilical to the ground station's link emulator as in the software-in-the-loop build (obc/devices/
** dev_radio.c), so the OBC runs on the bench before its radio board is fitted. A radio that fails later is
** restarted at most every 5 s; frames sent meanwhile fail, as on a real link.
*/
#include "dev_internal.h"
#include "fs_hal.h"
#include "radio_pico.h"
#include "sx1262.h"

#define FREQ_HZ  869525000u /* ICD 7.1 */
#define RETRY_US 5000000u

static sx1262_t radio;
static int      probed;
static int      fitted;        /* the SX1262 answered at start-up: frames go on the air */
static int      ready;         /* and it is working now */
static int8_t   power_dbm = 2; /* until obc_comms sets it */
static uint64_t retry_at_us;

static int start(void)
{
    int rc;

    radio.freq_hz   = FREQ_HZ;
    radio.power_dbm = power_dbm;
    rc              = sx1262_init(&radio);
    ready           = rc == SX1262_OK;
    retry_at_us     = fs_hal_time_us() + RETRY_US;
    return rc;
}

/* 1 if frames go on the air (whether or not the radio works at this moment), 0 if over the umbilical */
static int on_air(void)
{
    int rc;

    if (!probed)
    {
        probed = 1;
        pico_sx1262_bind(&radio);
        rc     = start();
        fitted = ready;
        if (fitted)
        {
            fs_hal_log("radio: SX1262 at %lu Hz, %d dBm", (unsigned long)FREQ_HZ, power_dbm);
        }
        else
        {
            fs_hal_log("no radio board (SX1262 error %d): RF frames go over the umbilical", rc);
        }
    }
    else if (fitted && !ready && fs_hal_time_us() >= retry_at_us)
    {
        rc = start();
        if (ready)
        {
            fs_hal_log("radio restarted");
        }
        else
        {
            fs_hal_log("radio restart failed (SX1262 error %d)", rc);
        }
    }
    return fitted;
}

int dev_radio_send(const uint8_t *frame, size_t len)
{
    if (!on_air())
    {
        return dev_map_umb(fs_umb_send_rf(frame, len));
    }
    if (!ready || sx1262_send(&radio, frame, len) != SX1262_OK)
    {
        return DEV_ERR_BUS;
    }
    return DEV_OK;
}

int dev_radio_set_power(int8_t dbm)
{
    power_dbm = dbm; /* kept for a restart */
    if (!on_air() || !ready)
    {
        return DEV_OK;
    }
    return sx1262_set_power(&radio, dbm) == SX1262_OK ? DEV_OK : DEV_ERR_BUS;
}

int dev_radio_poll(uint8_t *buf, size_t max, int16_t *rssi, int8_t *snr)
{
    size_t len = 0;
    int    ev;

    if (!on_air() || !ready)
    {
        return 0;
    }
    ev = sx1262_poll(&radio, buf, max, &len, rssi, snr);
    if (ev < 0)
    {
        fs_hal_log("radio error %d: restarting it in %u s", ev, (unsigned)(RETRY_US / 1000000u));
        ready       = 0;
        retry_at_us = fs_hal_time_us() + RETRY_US;
        return 0;
    }
    return ev == SX1262_EVENT_RX ? (int)len : 0;
}

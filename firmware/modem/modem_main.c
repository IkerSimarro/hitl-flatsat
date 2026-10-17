/*
** Ground modem: a Pico 2 with the Pico-LoRa-SX1262 board on the ground station PC's USB port (ICD 7.5, IF-05)
**
** A pure radio, with hil_link framing on the USB serial port:
**   RF_TX      ground software -> modem: one RF frame to transmit
**   RF_RX      modem -> ground software: one received RF frame; addr = RSSI (i16, dBm) | SNR (i8, 0.25 dB) << 16
**   HEARTBEAT  modem -> ground software, once a second: status OK while the radio works, BUS_ERROR while it doesn't
**              answer (it's retried every 5 s); addr = frames dropped so far
**   LOG        modem -> ground software: the modem's own messages
** It doesn't parse RF frames: contact windows, the airtime budget and every other link rule are the ground
** software's. One frame waits while another is on the air; a third is dropped and counted.
*/
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fs_hal.h"
#include "hil_link.h"
#include "radio_pico.h"
#include "sx1262.h"

#define FREQ_HZ        869525000u /* ICD 7.1 */
#define POWER_DBM      2
#define RETRY_INIT_US  5000000u
#define HEARTBEAT_US   1000000u

static sx1262_t      radio;
static int           radio_ok;
static hil_decoder_t decoder;
static hil_frame_t   rx_frame, tx_frame;
static uint8_t       wire[HIL_ENCODED_MAX];
static uint8_t       queued[256];
static size_t        queued_len;
static uint8_t       seq;
static uint32_t      dropped;
static int           init_rc = 1; /* the last radio start-up's result, so a repeated failure is logged once */

static void send(uint8_t type, uint8_t status, uint32_t addr, const uint8_t *payload, size_t len)
{
    size_t n;

    tx_frame.type   = type;
    tx_frame.bus    = 0;
    tx_frame.seq    = seq++;
    tx_frame.status = status;
    tx_frame.addr   = addr;
    tx_frame.len    = (uint16_t)len;
    memcpy(tx_frame.payload, payload, len);
    n = hil_encode(&tx_frame, wire, sizeof(wire));
    if (n > 0)
    {
        fs_hal_umb_write(wire, n);
    }
}

static void modem_log(const char *fmt, ...)
{
    char    text[120];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        send(HIL_LOG, HIL_STATUS_OK, 0, (const uint8_t *)text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
    }
    fs_hal_log("%s", text);
}

static void transmit(const uint8_t *frame, size_t len)
{
    if (!radio_ok)
    {
        dropped++;
        return;
    }
    if (radio.transmitting)
    {
        if (queued_len != 0)
        {
            dropped++; /* the ground software paces its uplink; this only happens if it doesn't */
            return;
        }
        memcpy(queued, frame, len);
        queued_len = len;
        return;
    }
    if (sx1262_send(&radio, frame, len) != SX1262_OK)
    {
        dropped++;
    }
}

static void from_ground(void)
{
    uint8_t buf[256];
    int     n = fs_hal_umb_read(buf, sizeof(buf));
    int     i;

    for (i = 0; i < n; i++)
    {
        if (hil_decoder_feed(&decoder, buf[i], &rx_frame) == HIL_DECODE_FRAME && rx_frame.type == HIL_RF_TX &&
            rx_frame.len > 0 && rx_frame.len <= 255)
        {
            transmit(rx_frame.payload, rx_frame.len);
        }
    }
}

static void from_air(void)
{
    uint8_t frame[256];
    size_t  len  = 0;
    int16_t rssi = 0;
    int8_t  snr  = 0;
    int     ev   = sx1262_poll(&radio, frame, sizeof(frame), &len, &rssi, &snr);

    if (ev == SX1262_EVENT_RX)
    {
        send(HIL_RF_RX, HIL_STATUS_OK, (uint32_t)(uint16_t)rssi | ((uint32_t)(uint8_t)snr << 16), frame, len);
    }
    else if (ev == SX1262_EVENT_TX_DONE && queued_len != 0)
    {
        size_t len_q = queued_len;
        queued_len   = 0;
        transmit(queued, len_q);
    }
    else if (ev < 0)
    {
        modem_log("radio error %d: restarting it", ev);
        radio_ok = 0;
    }
}

int main(int argc, char **argv)
{
    uint64_t next_init = 0, next_heartbeat = 0;

    if (fs_hal_init(argc, argv) != 0)
    {
        return 1;
    }
    hil_decoder_init(&decoder);
    pico_sx1262_bind(&radio);
    radio.freq_hz   = FREQ_HZ;
    radio.power_dbm = POWER_DBM;

    for (;;)
    {
        uint64_t now = fs_hal_time_us();

        if (!radio_ok && now >= next_init)
        {
            int rc    = sx1262_init(&radio);
            radio_ok  = rc == SX1262_OK;
            next_init = now + RETRY_INIT_US;
            if (radio_ok)
            {
                modem_log("radio ready: %u Hz, SF7, 125 kHz, %d dBm", (unsigned)FREQ_HZ, POWER_DBM);
            }
            else if (rc != init_rc)
            {
                modem_log("radio not answering (%d); retrying every %u s", rc, (unsigned)(RETRY_INIT_US / 1000000u));
            }
            init_rc = rc;
        }
        from_ground();
        if (radio_ok)
        {
            from_air();
        }
        if (now >= next_heartbeat)
        {
            next_heartbeat = now + HEARTBEAT_US;
            send(HIL_HEARTBEAT, radio_ok ? HIL_STATUS_OK : HIL_STATUS_BUS_ERROR, dropped, NULL, 0);
            fs_hal_watchdog_kick();
        }
        fs_hal_sleep_us(500);
    }
}

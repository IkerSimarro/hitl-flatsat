/*
** Radio (ICD 7.1): software-in-the-loop backend
**
** Frames go out over the umbilical (RF_TX) to the ground station's link emulator, and arrive the same way
** (RF_RX, handled in obc_main.c). The Pico build drives the SX1262 when its board is fitted, behind the same calls
** (hal/pico/dev_radio_pico.c).
*/
#include "dev_internal.h"

int dev_radio_send(const uint8_t *frame, size_t len)
{
    return dev_map_umb(fs_umb_send_rf(frame, len));
}

int dev_radio_set_power(int8_t dbm)
{
    (void)dbm; /* nothing to configure on the emulated link: the ground station models a flight link */
    return DEV_OK;
}

int dev_radio_poll(uint8_t *buf, size_t max, int16_t *rssi, int8_t *snr)
{
    (void)buf;
    (void)max;
    (void)rssi;
    (void)snr;
    return 0; /* frames arrive through the umbilical's handler */
}

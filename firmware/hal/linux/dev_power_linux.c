/*
** EPS power hardware on Linux (SIL): INA219 readings, charger pins and switch lines through the SIL harness
**
** INA219 behaviour: 4 mV bus voltage resolution, 0.1 mA current resolution with the planned 0.1 ohm shunt,
** plus +-1 LSB of noise.
*/
#include <stdlib.h>

#include "dev_power.h"
#include "flatsat_icd.h"
#include "sil_harness.h"

static sil_harness_t *h;

static int noise_lsb(void)
{
    return (rand() % 3) - 1;
}

int dev_power_init(void)
{
    h = sil_harness();
    srand(42);
    return h != NULL ? 0 : -1;
}

int dev_power_read_rail(uint8_t rail, uint16_t *mv, int16_t *ma)
{
    float v;
    float i;

    if (rail >= DEV_POWER_NUM_RAILS)
    {
        return -1;
    }
    v   = h->rail_v[rail];
    i   = h->rail_ma[rail];
    *mv = (uint16_t)((int)(v * 1000.0f / 4.0f + noise_lsb()) * 4);
    *ma = (int16_t)((int)(i * 10.0f + noise_lsb()) / 10);
    return 0;
}

void dev_power_set_switch(uint8_t sw, int on)
{
    if (sw == DEV_POWER_SW_ADCS_RUN)
    {
        h->adcs_run = (uint8_t)(on != 0);
    }
    else if (sw == DEV_POWER_SW_MOTOR)
    {
        h->motor_nsleep = (uint8_t)(on != 0);
    }
}

int dev_power_get_switch(uint8_t sw)
{
    if (sw == DEV_POWER_SW_ADCS_RUN)
    {
        return h->adcs_run;
    }
    if (sw == DEV_POWER_SW_MOTOR)
    {
        return h->motor_nsleep;
    }
    return 0;
}

uint8_t dev_power_charge_state(void)
{
    if (h->charger_chrg)
    {
        return FLATSAT_CHARGE_STATE_CHARGING;
    }
    return h->charger_stdby ? FLATSAT_CHARGE_STATE_FULL : FLATSAT_CHARGE_STATE_IDLE;
}

/*
** Register-level model of INA219 monitors (see fake_ina219.h), from datasheet SBOS448
*/
#include "fake_ina219.h"

#include <math.h>
#include <string.h>

#include "ina219.h"

fake_ina219_t fake_ina219[3];

void fake_ina219_reset(void)
{
    static const uint8_t addrs[3] = {0x40, 0x41, 0x44};
    unsigned             i;

    memset(fake_ina219, 0, sizeof(fake_ina219));
    for (i = 0; i < 3; i++)
    {
        fake_ina219[i].addr                      = addrs[i];
        fake_ina219[i].present                   = 1;
        fake_ina219[i].regs[INA219_REG_CONFIG] = 0x399F; /* power-on default */
    }
}

static fake_ina219_t *find(uint8_t addr)
{
    unsigned i;

    for (i = 0; i < 3; i++)
    {
        if (fake_ina219[i].present && fake_ina219[i].addr == addr)
        {
            return &fake_ina219[i];
        }
    }
    return NULL; /* no acknowledge */
}

/* The measurement registers, from the true values: shunt LSB 10 uV, bus LSB 4 mV, current = shunt x CAL / 4096 */
static void convert(fake_ina219_t *d)
{
    double   shunt = d->true_ma * 0.1 / 0.01; /* mA x 0.1 ohm, in 10 uV steps */
    int      ovf   = fabs(shunt) > 8000.0;    /* beyond +-80 mV */
    long     s     = lround(shunt > 8000.0 ? 8000.0 : shunt < -8000.0 ? -8000.0 : shunt);
    long     bus   = lround(d->true_mv / 4.0);

    d->regs[INA219_REG_SHUNT]   = (uint16_t)(int16_t)s;
    d->regs[INA219_REG_BUS]     = (uint16_t)((bus << 3) | 0x0002 | (ovf ? 0x0001 : 0)); /* CNVR, OVF */
    d->regs[INA219_REG_CURRENT] = (uint16_t)(int16_t)(s * d->regs[INA219_REG_CAL] / 4096);
}

int fake_ina219_write(void *ctx, uint8_t addr, const uint8_t *data, size_t len)
{
    fake_ina219_t *d = find(addr);

    (void)ctx;
    if (d == NULL || len < 1)
    {
        return -1;
    }
    d->ptr = data[0];
    if (len == 3 && d->ptr <= INA219_REG_CAL && !d->ignore_writes &&
        (d->ptr == INA219_REG_CONFIG || d->ptr == INA219_REG_CAL))
    {
        d->regs[d->ptr] = (uint16_t)((data[1] << 8) | data[2]);
    }
    return 0;
}

int fake_ina219_read(void *ctx, uint8_t addr, uint8_t *data, size_t len)
{
    fake_ina219_t *d = find(addr);

    (void)ctx;
    if (d == NULL || len != 2 || d->ptr > INA219_REG_CAL)
    {
        return -1;
    }
    convert(d);
    data[0] = (uint8_t)(d->regs[d->ptr] >> 8);
    data[1] = (uint8_t)d->regs[d->ptr];
    return 0;
}

/*
** INA219 current and voltage monitor (see ina219.h)
*/
#include "ina219.h"

#define BUS_OVF 0x0001 /* bus register: the power or current calculation overflowed */

static int write_reg(ina219_t *d, uint8_t reg, uint16_t value)
{
    const uint8_t data[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return d->write(d->ctx, d->addr, data, sizeof(data)) == 0 ? INA219_OK : INA219_ERR_I2C;
}

static int read_reg(ina219_t *d, uint8_t reg, uint16_t *value)
{
    uint8_t data[2];

    if (d->write(d->ctx, d->addr, &reg, 1) != 0 || d->read(d->ctx, d->addr, data, 2) != 0)
    {
        return INA219_ERR_I2C;
    }
    *value = (uint16_t)((data[0] << 8) | data[1]);
    return INA219_OK;
}

int ina219_init(ina219_t *d)
{
    uint16_t config = 0;
    int      rc     = write_reg(d, INA219_REG_CONFIG, INA219_CONFIG);

    if (rc == INA219_OK)
    {
        rc = write_reg(d, INA219_REG_CAL, INA219_CAL);
    }
    if (rc == INA219_OK)
    {
        rc = read_reg(d, INA219_REG_CONFIG, &config);
    }
    if (rc != INA219_OK)
    {
        return rc;
    }
    return config == INA219_CONFIG ? INA219_OK : INA219_ERR_CHIP;
}

int ina219_read(ina219_t *d, uint16_t *bus_mv, int16_t *current_ma)
{
    uint16_t bus;
    uint16_t raw;
    int32_t  tenths;
    int      rc = read_reg(d, INA219_REG_BUS, &bus);

    if (rc == INA219_OK)
    {
        rc = read_reg(d, INA219_REG_CURRENT, &raw);
    }
    if (rc != INA219_OK)
    {
        return rc;
    }
    *bus_mv = (uint16_t)((bus >> 3) * 4u);        /* bits 15-3, 4 mV each */
    tenths  = (int16_t)raw;                       /* 0.1 mA each */
    *current_ma = (int16_t)((tenths + (tenths >= 0 ? 5 : -5)) / 10);
    return (bus & BUS_OVF) ? INA219_ERR_RANGE : INA219_OK;
}

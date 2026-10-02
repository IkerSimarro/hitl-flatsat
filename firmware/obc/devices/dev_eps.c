/*
** EPS driver (generic_eps over I2C)
**
** Every write is {command, value, CRC-8}. Command 0x70 requests housekeeping: a 64-byte big-endian
** reply plus a CRC-8. Commands 0..7 set a switch to 0xAA (on) or 0x00 (off). CRC-8 is polynomial 0x31,
** initial value 0xFF. Raw units: voltages and currents in milli-units; temperatures (T + 60) * 100.
*/
#include "dev_internal.h"

#define EPS_HK_CMD     0x70
#define EPS_HK_LEN     64
#define EPS_SWITCH_ON  0xAA
#define EPS_SWITCH_OFF 0x00
#define EPS_SW_OFFSET  16
#define EPS_SW_LEN     6

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0xFF;
    size_t  i;
    int     b;

    for (i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static float milli(const uint8_t *p)
{
    return (float)dev_be16(p) / 1000.0f;
}

static float temp_c(const uint8_t *p)
{
    return (float)dev_be16(p) / 100.0f - 60.0f;
}

int dev_eps_read(dev_eps_t *out)
{
    uint8_t cmd[3] = {EPS_HK_CMD, 0, 0};
    uint8_t rx[EPS_HK_LEN + 1];
    int     rc;
    int     i;

    cmd[2] = crc8(cmd, 2);
    rc     = fs_umb_i2c(DEV_EPS_I2C_BUS, DEV_EPS_I2C_ADDR, cmd, sizeof(cmd), rx, sizeof(rx), DEV_TXN_TIMEOUT_MS);
    if (rc != FS_UMB_OK)
    {
        return dev_map_umb(rc);
    }
    if (crc8(rx, EPS_HK_LEN) != rx[EPS_HK_LEN])
    {
        return DEV_ERR_FORMAT;
    }

    out->batt_v      = milli(&rx[0]);
    out->batt_temp_c = temp_c(&rx[2]);
    out->bus_3v3_v   = milli(&rx[4]);
    out->bus_5v0_v   = milli(&rx[6]);
    out->bus_12v_v   = milli(&rx[8]);
    out->eps_temp_c  = temp_c(&rx[10]);
    out->sa_v        = milli(&rx[12]);
    out->sa_temp_c   = temp_c(&rx[14]);
    out->switch_mask = 0;
    for (i = 0; i < DEV_NUM_EPS_SWITCHES; i++)
    {
        const uint8_t *sw = &rx[EPS_SW_OFFSET + EPS_SW_LEN * i];
        out->switch_v[i]  = milli(&sw[0]);
        out->switch_a[i]  = milli(&sw[2]);
        if (sw[5] == EPS_SWITCH_ON) /* status low byte */
        {
            out->switch_mask |= (uint8_t)(1u << i);
        }
    }
    return DEV_OK;
}

int dev_eps_set_switch(uint8_t sw, int on)
{
    uint8_t   cmd[3];
    dev_eps_t hk;
    int       rc;

    if (sw >= DEV_NUM_EPS_SWITCHES)
    {
        return DEV_ERR_ARG;
    }
    cmd[0] = sw;
    cmd[1] = on ? EPS_SWITCH_ON : EPS_SWITCH_OFF;
    cmd[2] = crc8(cmd, 2);
    rc     = fs_umb_i2c(DEV_EPS_I2C_BUS, DEV_EPS_I2C_ADDR, cmd, sizeof(cmd), NULL, 0, DEV_TXN_TIMEOUT_MS);
    if (rc != FS_UMB_OK)
    {
        return dev_map_umb(rc);
    }

    /* Confirm in housekeeping, as NOS3's cFS app does */
    rc = dev_eps_read(&hk);
    if (rc != DEV_OK)
    {
        return rc;
    }
    return (((hk.switch_mask >> sw) & 1u) == (on ? 1u : 0u)) ? DEV_OK : DEV_ERR_FORMAT;
}

/*
** Coarse sun sensor driver (generic_css over I2C)
**
** A 12-byte read returns six big-endian u16 values, each the normalised illumination times 1000
** (0 when that sensor reports invalid).
*/
#include "dev_internal.h"

#define CSS_DATA_LEN (2 * DEV_NUM_CSS)

int dev_css_read(dev_css_t *out)
{
    uint8_t rx[CSS_DATA_LEN];
    int     rc;
    int     i;

    rc = fs_umb_i2c(DEV_CSS_I2C_BUS, DEV_CSS_I2C_ADDR, NULL, 0, rx, sizeof(rx), DEV_TXN_TIMEOUT_MS);
    if (rc != FS_UMB_OK)
    {
        return dev_map_umb(rc);
    }
    for (i = 0; i < DEV_NUM_CSS; i++)
    {
        out->illum[i] = (float)dev_be16(&rx[2 * i]) / 1000.0f;
    }
    return DEV_OK;
}

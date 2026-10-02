/*
** Magnetometer driver (generic_mag over SPI)
**
** A 16-byte read returns {DE AD BE EF} and three big-endian offset-binary u32 values in nanotesla:
**   field_nT = (raw - 21474 * 100000) / 21474
** NOS3's cFS driver does this in 32-bit integer arithmetic, which overflows for raw values above
** INT32_MAX (fields above about +60 uT); this driver uses 64-bit arithmetic instead.
*/
#include "dev_internal.h"

#define MAG_FRAME_LEN 16
#define MAG_CONV      21474
#define MAG_RANGE     100000

int dev_mag_read(dev_mag_t *out)
{
    uint8_t rx[MAG_FRAME_LEN];
    int     rc;
    int     i;

    rc = fs_umb_spi(DEV_MAG_SPI_BUS, DEV_MAG_SPI_CS, NULL, 0, rx, sizeof(rx), DEV_TXN_TIMEOUT_MS);
    if (rc != FS_UMB_OK)
    {
        return dev_map_umb(rc);
    }
    if (rx[0] != 0xDE || rx[1] != 0xAD || rx[2] != 0xBE || rx[3] != 0xEF)
    {
        return DEV_ERR_FORMAT;
    }

    for (i = 0; i < 3; i++)
    {
        int64_t raw = (int64_t)dev_be32(&rx[4 + 4 * i]);
        double  nt  = (double)(raw - (int64_t)MAG_CONV * MAG_RANGE) / MAG_CONV;
        out->field[i] = (float)(nt * 1e-9);
    }
    return DEV_OK;
}

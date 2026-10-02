/*
** Fine sun sensor driver (generic_fss over SPI)
**
** Two transactions, as in NOS3's cFS driver:
**   1. command  {DE AD BE EF, 0x01 (request data), 0x01, checksum}
**   2. 16-byte read {DE AD BE EF, 0x01, 0x0A, alpha f32 LE, beta f32 LE, error code, checksum}
** Checksums are the low byte of the sum of the bytes after the header.
*/
#include <string.h>

#include "dev_internal.h"

#define FSS_CMD_LEN      7
#define FSS_DATA_LEN     16
#define FSS_REQ_DATA_CMD 0x01

static uint8_t sum8(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    size_t   i;

    for (i = 0; i < n; i++)
    {
        s += p[i];
    }
    return (uint8_t)(s & 0xFF);
}

static float le_float(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float    f;

    memcpy(&f, &u, sizeof(f));
    return f;
}

int dev_fss_read(dev_fss_t *out)
{
    uint8_t cmd[FSS_CMD_LEN] = {0xDE, 0xAD, 0xBE, 0xEF, FSS_REQ_DATA_CMD, 0x01, 0};
    uint8_t echo[FSS_CMD_LEN];
    uint8_t zeros[FSS_DATA_LEN];
    uint8_t rx[FSS_DATA_LEN];
    int     rc;

    cmd[6] = sum8(&cmd[4], 2);
    rc     = fs_umb_spi(DEV_FSS_SPI_BUS, DEV_FSS_SPI_CS, cmd, sizeof(cmd), echo, sizeof(echo), DEV_TXN_TIMEOUT_MS);
    if (rc != FS_UMB_OK)
    {
        return dev_map_umb(rc);
    }

    memset(zeros, 0, sizeof(zeros));
    rc = fs_umb_spi(DEV_FSS_SPI_BUS, DEV_FSS_SPI_CS, zeros, sizeof(zeros), rx, sizeof(rx), DEV_TXN_TIMEOUT_MS);
    if (rc != FS_UMB_OK)
    {
        return dev_map_umb(rc);
    }

    if (rx[0] != 0xDE || rx[1] != 0xAD || rx[2] != 0xBE || rx[3] != 0xEF || rx[4] != 0x01 || rx[5] != 0x0A ||
        rx[15] != sum8(&rx[4], 11))
    {
        return DEV_ERR_FORMAT;
    }

    out->alpha = le_float(&rx[6]);
    out->beta  = le_float(&rx[10]);
    out->error = rx[14];
    return DEV_OK;
}

/*
** Star tracker driver (generic_star_tracker over UART)
**
** Request: {DE AD, 0x02 (request data), 0 0 0 0, BE EF}. The device echoes the 9-byte command, then
** sends {DE AD, q0..q3 as big-endian offset-binary u16, valid, BE EF}, where q = (raw - 32768) / 32767.
*/
#include <string.h>

#include "dev_internal.h"

#define ST_CMD_LEN      9
#define ST_DATA_LEN     13
#define ST_REQ_DATA_CMD 0x02
#define ST_TIMEOUT_MS   100
#define ST_QUIET_MS     20

int dev_st_read(dev_st_t *out)
{
    static const uint8_t cmd[ST_CMD_LEN] = {0xDE, 0xAD, ST_REQ_DATA_CMD, 0, 0, 0, 0, 0xBE, 0xEF};
    uint8_t              rx[ST_CMD_LEN + ST_DATA_LEN];
    const uint8_t       *d;
    int                  n;
    int                  i;

    n = dev_uart_request(DEV_ST_UART, cmd, sizeof(cmd), rx, sizeof(rx), sizeof(rx), ST_TIMEOUT_MS, ST_QUIET_MS);
    if (n < 0)
    {
        return n;
    }
    if (n != (int)sizeof(rx) || memcmp(rx, cmd, ST_CMD_LEN) != 0)
    {
        return DEV_ERR_FORMAT;
    }

    d = &rx[ST_CMD_LEN];
    if (d[0] != 0xDE || d[1] != 0xAD || d[11] != 0xBE || d[12] != 0xEF)
    {
        return DEV_ERR_FORMAT;
    }
    for (i = 0; i < 4; i++)
    {
        out->q[i] = (float)(((double)dev_be16(&d[2 + 2 * i]) - 32768.0) / 32767.0);
    }
    out->valid = d[10];
    return DEV_OK;
}

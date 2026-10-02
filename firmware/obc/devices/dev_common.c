/*
** Helpers shared by the OBC device drivers
*/
#include "dev_internal.h"

#include "fs_hal.h"

int dev_map_umb(int rc)
{
    if (rc == FS_UMB_OK)
    {
        return DEV_OK;
    }
    return rc == FS_UMB_TIMEOUT ? DEV_ERR_TIMEOUT : DEV_ERR_BUS;
}

int dev_uart_request(uint8_t bus, const uint8_t *req, size_t req_len, uint8_t *reply, size_t reply_max,
                     size_t expected, uint32_t first_byte_timeout_ms, uint32_t quiet_ms)
{
    uint64_t start;
    uint64_t last_rx = 0;
    size_t   got     = 0;

    /* Anything already buffered belongs to an earlier exchange */
    fs_umb_poll();
    fs_umb_uart_flush(bus);

    if (fs_umb_uart_write(bus, req, req_len) != FS_UMB_OK)
    {
        return DEV_ERR_BUS;
    }

    start = fs_hal_time_us();
    for (;;)
    {
        size_t n;
        uint64_t now;

        fs_umb_poll();
        n = fs_umb_uart_read(bus, reply + got, reply_max - got);
        now = fs_hal_time_us();
        if (n > 0)
        {
            got += n;
            last_rx = now;
        }

        if (got == reply_max || (expected > 0 && got >= expected))
        {
            break;
        }
        if (got == 0 && now - start > (uint64_t)first_byte_timeout_ms * 1000u)
        {
            return DEV_ERR_TIMEOUT;
        }
        if (got > 0 && now - last_rx > (uint64_t)quiet_ms * 1000u)
        {
            break;
        }
        fs_hal_sleep_us(200);
    }
    return (int)got;
}

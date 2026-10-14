/*
** Helpers shared by the OBC device drivers
*/
#include <string.h>

#include "dev_internal.h"

#include "fs_hal.h"

int dev_open_all(void)
{
    int rc = FS_UMB_OK;
    int i;

    rc |= fs_umb_can_open(DEV_IMU_CAN_BUS);
    rc |= fs_umb_spi_open(DEV_MAG_SPI_BUS, DEV_MAG_SPI_CS);
    rc |= fs_umb_spi_open(DEV_FSS_SPI_BUS, DEV_FSS_SPI_CS);
    rc |= fs_umb_i2c_open(DEV_CSS_I2C_BUS);
    rc |= fs_umb_i2c_open(DEV_EPS_I2C_BUS);
    rc |= fs_umb_uart_open(DEV_ST_UART);
    for (i = 0; i < DEV_NUM_RW; i++)
    {
        rc |= fs_umb_uart_open((uint8_t)(DEV_RW_UART_BASE + i));
    }
    /* The GPS port is opened by dev_gps_init() */
    return rc == FS_UMB_OK ? DEV_OK : DEV_ERR_BUS;
}

int dev_map_umb(int rc)
{
    if (rc == FS_UMB_OK)
    {
        return DEV_OK;
    }
    return rc == FS_UMB_TIMEOUT ? DEV_ERR_TIMEOUT : DEV_ERR_BUS;
}

static uint32_t uart_count;
static uint64_t uart_first_total_us, uart_total_us;
static uint32_t uart_first_max_us, uart_max_us;

static void uart_timing(uint64_t start, uint64_t first, uint64_t end)
{
    uint32_t f = (uint32_t)(first - start), t = (uint32_t)(end - start);

    uart_count++;
    uart_first_total_us += f;
    uart_total_us += t;
    uart_first_max_us = f > uart_first_max_us ? f : uart_first_max_us;
    uart_max_us       = t > uart_max_us ? t : uart_max_us;
}

void dev_take_uart_timing(dev_uart_timing_t *t)
{
    t->count         = uart_count;
    t->first_mean_us = uart_count ? (uint32_t)(uart_first_total_us / uart_count) : 0;
    t->first_max_us  = uart_first_max_us;
    t->total_mean_us = uart_count ? (uint32_t)(uart_total_us / uart_count) : 0;
    t->total_max_us  = uart_max_us;
    uart_count = 0;
    uart_first_total_us = uart_total_us = 0;
    uart_first_max_us = uart_max_us = 0;
}

int dev_uart_request(uint8_t bus, const uint8_t *req, size_t req_len, uint8_t *reply, size_t reply_max,
                     size_t expected, uint32_t first_byte_timeout_ms, uint32_t quiet_ms)
{
    uint64_t start;
    uint64_t first   = 0;
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
            first = got == 0 ? now : first;
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
    uart_timing(start, first, fs_hal_time_us());
    return (int)got;
}

void dev_uart_request_all(unsigned n, const uint8_t bus[], const char *const req[], uint8_t *const reply[],
                          size_t reply_max, uint32_t first_byte_timeout_ms, uint32_t quiet_ms, int rc[])
{
    uint64_t start;
    uint64_t first[DEV_UART_MAX_PARALLEL]   = {0};
    uint64_t last_rx[DEV_UART_MAX_PARALLEL] = {0};
    size_t   got[DEV_UART_MAX_PARALLEL]     = {0};
    int      open[DEV_UART_MAX_PARALLEL]    = {0};
    unsigned i, pending = 0;

    /* Anything already buffered belongs to an earlier exchange */
    fs_umb_poll();
    for (i = 0; i < n && i < DEV_UART_MAX_PARALLEL; i++)
    {
        fs_umb_uart_flush(bus[i]);
        rc[i] = DEV_ERR_BUS;
        if (fs_umb_uart_write(bus[i], (const uint8_t *)req[i], strlen(req[i])) == FS_UMB_OK)
        {
            open[i] = 1;
            pending++;
        }
    }

    start = fs_hal_time_us();
    while (pending > 0)
    {
        uint64_t now;

        fs_umb_poll();
        now = fs_hal_time_us();
        for (i = 0; i < n && i < DEV_UART_MAX_PARALLEL; i++)
        {
            size_t got_now;

            if (!open[i])
            {
                continue;
            }
            got_now = fs_umb_uart_read(bus[i], reply[i] + got[i], reply_max - got[i]);
            if (got_now > 0)
            {
                first[i] = got[i] == 0 ? now : first[i];
                got[i] += got_now;
                last_rx[i] = now;
            }
            if (got[i] == 0 && now - start > (uint64_t)first_byte_timeout_ms * 1000u)
            {
                rc[i]   = DEV_ERR_TIMEOUT;
                open[i] = 0;
                pending--;
            }
            else if (got[i] == reply_max || (got[i] > 0 && now - last_rx[i] > (uint64_t)quiet_ms * 1000u))
            {
                rc[i]   = (int)got[i];
                open[i] = 0;
                pending--;
                uart_timing(start, first[i], now);
            }
        }
        if (pending > 0)
        {
            fs_hal_sleep_us(200);
        }
    }
}


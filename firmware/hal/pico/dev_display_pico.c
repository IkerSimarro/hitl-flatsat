/*
** Status display (obc_devices.h): the SH1106 OLED on I2C0 (GP8 SDA, GP9 SCL, 400 kHz), address 0x3C
**
** dev_display_show draws the text into a frame; dev_display_service writes one changed page (a text line) per call,
** about 3 ms, so a refresh never holds the OBC for long. A display that doesn't answer is looked for again every
** 10 s, so one plugged in late, or reset by a glitch, comes back.
*/
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#include "fs_hal.h"
#include "obc_devices.h"
#include "sh1106.h"

#define I2C_PORT       i2c0
#define I2C_HZ         400000
#define I2C_TIMEOUT_US 10000 /* a page is 129 bytes: about 3 ms */
#define PIN_SDA        8
#define PIN_SCL        9
#define OLED_ADDR      0x3C
#define RETRY_US       10000000u

static sh1106_t       oled;
static sh1106_frame_t frame, shown; /* what should be on the display, and what is */
static uint8_t        dirty;        /* pages to write */
static int            bus_up, ready, reported;
static uint64_t       retry_at_us;

static int i2c_write_cb(void *ctx, uint8_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    return i2c_write_timeout_us(I2C_PORT, addr, data, len, false, I2C_TIMEOUT_US) == (int)len ? 0 : -1;
}

static void start(void)
{
    if (!bus_up)
    {
        i2c_init(I2C_PORT, I2C_HZ);
        gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
        gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
        gpio_pull_up(PIN_SDA); /* the breakout has its own pull-ups too */
        gpio_pull_up(PIN_SCL);
        oled   = (sh1106_t){.write = i2c_write_cb, .ctx = NULL, .addr = OLED_ADDR};
        bus_up = 1;
    }
    retry_at_us = time_us_64() + RETRY_US;
    ready       = sh1106_init(&oled) == SH1106_OK;
    if (ready)
    {
        memset(shown, 0, sizeof(shown)); /* sh1106_init cleared it */
        fs_hal_log("status display: SH1106 OLED");
        reported = 0;
    }
    else if (!reported)
    {
        fs_hal_log("no status display (OLED not answering); looking again every %u s", (unsigned)(RETRY_US / 1000000u));
        reported = 1;
    }
}

int dev_display_show(dev_display_text_t text)
{
    unsigned i;

    if (!ready && time_us_64() >= retry_at_us)
    {
        start();
    }
    if (!ready)
    {
        return DEV_ERR_BUS;
    }
    for (i = 0; i < DEV_DISPLAY_LINES; i++)
    {
        sh1106_text(frame, i, text[i]);
        if (memcmp(frame[i], shown[i], SH1106_WIDTH) != 0)
        {
            dirty |= (uint8_t)(1u << i);
        }
    }
    return DEV_OK;
}

void dev_display_service(void)
{
    unsigned page = 0;

    if (!ready || dirty == 0)
    {
        return;
    }
    while (!(dirty & (1u << page)))
    {
        page++;
    }
    if (sh1106_write_page(&oled, page, frame[page]) != SH1106_OK)
    {
        fs_hal_log("status display stopped answering; looking again every %u s", (unsigned)(RETRY_US / 1000000u));
        ready       = 0;
        reported    = 1;
        retry_at_us = time_us_64() + RETRY_US;
        return;
    }
    memcpy(shown[page], frame[page], SH1106_WIDTH);
    dirty &= (uint8_t)~(1u << page);
}

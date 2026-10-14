/*
** FlatSat HAL, Raspberry Pi Pico 2 (RP2350) backend
**
**   umbilical  USB CDC (the SDK's USB serial driver, used as a raw byte channel: nothing else may print
**              to it). The host side opens it as /dev/ttyACM0 and the HIL bridge attaches (ICD IF-01).
**              Setting the port to 1200 baud restarts the Pico in its USB bootloader, for reflashing.
**   log        UART0 at 115200 baud on GP0 (TX) / GP1 (RX), and a LOG frame on the umbilical while it is up
**   watchdog   the hardware watchdog; fs_hal_watchdog_kick() must run every WATCHDOG_MS
**   reset cause  from the watchdog: a timeout, a commanded reboot (marked in a scratch register) or power-on
**              (power, the RUN pin or the reset button)
**   CAN        not yet: the MCP2515 driver comes with the CAN boards. Until then the bus reports itself as
**              not fitted, and the OBC runs on its own.
*/
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/binary_info.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "flatsat_icd.h"
#include "fs_hal.h"
#include "fs_umbilical.h"

#define LOG_UART      uart0
#define LOG_UART_TX   0
#define LOG_UART_RX   1
#define LOG_BAUD      115200
#define WATCHDOG_MS   3000u
#define REBOOT_MAGIC  0x464C5352u /* "FLSR" in watchdog scratch 0: the last reboot was commanded */

static uint8_t reset_cause;

/* Shown by picotool info: which board this is, and what its pins do */
bi_decl(bi_program_description("HITL FlatSat OBC flight software (umbilical: USB)"));
bi_decl(bi_2pins_with_func(LOG_UART_TX, LOG_UART_RX, GPIO_FUNC_UART));

int fs_hal_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (watchdog_hw->scratch[0] == REBOOT_MAGIC)
    {
        reset_cause = FLATSAT_RESET_CAUSE_COMMAND;
    }
    else if (watchdog_enable_caused_reboot())
    {
        reset_cause = FLATSAT_RESET_CAUSE_WATCHDOG;
    }
    else if (watchdog_caused_reboot())
    {
        reset_cause = FLATSAT_RESET_CAUSE_UNKNOWN; /* e.g. picotool or the bootloader restarting the chip */
    }
    else
    {
        reset_cause = FLATSAT_RESET_CAUSE_POWER_ON;
    }
    watchdog_hw->scratch[0] = 0;

    uart_init(LOG_UART, LOG_BAUD);
    gpio_set_function(LOG_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(LOG_UART_RX, GPIO_FUNC_UART);

    if (!stdio_usb_init())
    {
        return -1;
    }
    watchdog_enable(WATCHDOG_MS, true);
    return 0;
}

/* ---- Time ---- */

uint64_t fs_hal_time_us(void)
{
    return time_us_64();
}

void fs_hal_sleep_us(uint32_t us)
{
    sleep_us(us);
}

/* ---- Umbilical ---- */

int fs_hal_umb_write(const uint8_t *data, size_t len)
{
    if (!stdio_usb_connected())
    {
        return -1; /* nobody has the port open: drop, as a disconnected serial line would */
    }
    stdio_usb.out_chars((const char *)data, (int)len);
    return (int)len;
}

int fs_hal_umb_read(uint8_t *buf, size_t max)
{
    int n = stdio_usb.in_chars((char *)buf, (int)max);
    return n > 0 ? n : 0;
}

/* ---- CAN (not fitted yet) ---- */

int fs_hal_can_present(void)
{
    return 0;
}

int fs_hal_can_send(const fs_can_frame_t *frame)
{
    (void)frame;
    return -1;
}

int fs_hal_can_recv(fs_can_frame_t *frame)
{
    (void)frame;
    return 0;
}

void fs_hal_can_error_counters(uint8_t *tec, uint8_t *rec)
{
    *tec = 0;
    *rec = 0;
}

/* ---- System ---- */

uint8_t fs_hal_reset_cause(void)
{
    return reset_cause;
}

void fs_hal_reboot(void)
{
    fs_hal_log("rebooting");
    watchdog_hw->scratch[0] = REBOOT_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;)
    {
        tight_loop_contents();
    }
}

void fs_hal_check_power(void)
{
    /* The RUN pin resets the chip itself */
}

void fs_hal_watchdog_kick(void)
{
    watchdog_update();
}

void fs_hal_log(const char *fmt, ...)
{
    static int in_log; /* the umbilical may log while sending, and must not recurse */
    char       line[120];
    uint64_t   ms = time_us_64() / 1000u;
    va_list    ap;
    int        n;

    n = snprintf(line, sizeof(line), "[%6lu.%03lu] obc: ", (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u));
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    uart_puts(LOG_UART, line);
    uart_puts(LOG_UART, "\r\n");
    if (!in_log && fs_umb_link_up())
    {
        in_log = 1;
        fs_umb_log("%s", line + n);
        in_log = 0;
    }
}

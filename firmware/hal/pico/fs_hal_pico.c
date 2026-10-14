/*
** FlatSat HAL, Raspberry Pi Pico 2 (RP2350) backend
**
**   umbilical  USB CDC (the SDK's USB serial driver, used as a raw byte channel: nothing else may print
**              to it). The host side opens it as /dev/ttyACM0 and the HIL bridge attaches (ICD IF-01).
**              Setting the port to 1200 baud restarts the Pico in its USB bootloader, for reflashing.
**   log        UART0 at 115200 baud on GP0 (TX) / GP1 (RX), through a buffer that is drained without ever waiting
**              (a blocking write held the OBC ~17 ms per line, NCR-018), and a LOG frame on the umbilical while
**              it is up
**   watchdog   the hardware watchdog; fs_hal_watchdog_kick() must run every WATCHDOG_MS
**   LED        the on-board LED changes state at every watchdog kick: blinking means the software is running
**   reset cause  from the watchdog: a timeout, a commanded reboot (marked in a scratch register) or power-on
**              (power, the RUN pin or the reset button)
**   CAN        carried over the umbilical by the bridge, when it announces the bus (bridge --flatsat-can, ICD 8.1
**              CANBUS_*): the Pico without its CAN board talks to the SIL's CAN nodes. Otherwise the bus isn't
**              fitted and the OBC runs on its own. The MCP2515 driver comes with the CAN boards.
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

/* UART log buffer: written by fs_hal_log, drained whenever the UART can take characters */
#define LOG_RING 4096
static char     log_ring[LOG_RING];
static unsigned log_head, log_tail; /* head: next free, tail: next to send */

static void log_put(const char *s)
{
    for (; *s; s++)
    {
        unsigned next = (log_head + 1) % LOG_RING;
        if (next == log_tail)
        {
            return; /* full: the rest of the line is lost, the OBC doesn't wait */
        }
        log_ring[log_head] = *s;
        log_head           = next;
    }
}

static void log_drain(void)
{
    while (log_tail != log_head && uart_is_writable(LOG_UART))
    {
        uart_putc_raw(LOG_UART, log_ring[log_tail]);
        log_tail = (log_tail + 1) % LOG_RING;
    }
}

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

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
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
    log_drain();
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

/* ---- CAN: over the umbilical until the CAN board is fitted ---- */

int fs_hal_can_present(void)
{
    return fs_umb_canbus_present();
}

int fs_hal_can_send(const fs_can_frame_t *frame)
{
    return fs_umb_canbus_send(frame) == FS_UMB_OK ? 0 : -1;
}

int fs_hal_can_recv(fs_can_frame_t *frame)
{
    return fs_umb_canbus_recv(frame);
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
    gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
}

void fs_hal_log(const char *fmt, ...)
{
    static int in_log; /* the umbilical may log while sending, and must not recurse */
    char       line[256];
    uint64_t   ms = time_us_64() / 1000u;
    va_list    ap;
    int        n;

    n = snprintf(line, sizeof(line), "[%6lu.%03lu] obc: ", (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u));
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    log_put(line);
    log_put("\r\n");
    log_drain();
    if (!in_log && fs_umb_link_up())
    {
        in_log = 1;
        fs_umb_log("%s", line + n);
        in_log = 0;
    }
}

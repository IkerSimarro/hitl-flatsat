/*
** FlatSat HAL, Raspberry Pi Pico 2 (RP2350) backend: every board's firmware (the OBC, the ADCS and EPS nodes, the
** ground modem; FS_HAL_NAME names it in the log)
**
**   umbilical  USB CDC (the SDK's USB serial driver, used as a raw byte channel: nothing else may print
**              to it). The host side opens it as /dev/ttyACM0 and the HIL bridge attaches (ICD IF-01).
**              Setting the port to 1200 baud restarts the Pico in its USB bootloader, for reflashing.
**   log        UART0 at 115200 baud on GP0 (TX) / GP1 (RX), through a buffer that is drained without ever waiting
**              (a blocking write held the OBC ~17 ms per line, NCR-018), and a LOG frame on the umbilical while
**              it is up. The CAN nodes (FS_HAL_USB_LOG) use their USB port for nothing else, so their log also goes
**              there as plain text, for any serial terminal; it is kept from start-up until a terminal opens the
**              port, so the start-up messages aren't lost.
**   watchdog   the hardware watchdog; fs_hal_watchdog_kick() must run every WATCHDOG_MS
**   LED        the on-board LED changes state at every watchdog kick: blinking means the software is running
**   resources  idle time (time in fs_hal_sleep_us) and the deepest stack use: at start-up the memory the stack
**              can grow into is filled with a pattern, and the deepest word no longer holding it marks the
**              high-water mark. 4 KB are reserved below __StackTop (PICO_STACK_SIZE, the whole scratch bank Y);
**              past that the stack would grow on through scratch bank X, also empty, down to __StackLimit
**   reset cause  from the watchdog: a timeout, a commanded reboot (marked in a scratch register) or power-on
**              (power, the RUN pin or the reset button)
**   CAN        the Pico-CAN-B board (MCP2515 on SPI0: GP4 MISO, GP5 CS, GP6 SCK, GP7 MOSI; its interrupt line
**              isn't used, the controller is polled) when it answers at start-up. Without it, the bus is carried
**              over the umbilical by the bridge when it announces it (bridge --flatsat-can, ICD 8.1 CANBUS_*): the
**              OBC without its CAN board talks to the SIL's CAN nodes. Otherwise the bus isn't fitted.
*/
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/binary_info.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "flatsat_icd.h"
#include "fs_hal.h"
#include "fs_umbilical.h"
#include "mcp2515.h"

#define LOG_UART      uart0
#define LOG_UART_TX   0
#define LOG_UART_RX   1
#define LOG_BAUD      115200
#define WATCHDOG_MS   3000u
#define REBOOT_MAGIC  0x464C5352u /* "FLSR" in watchdog scratch 0: the last reboot was commanded */
#ifndef FS_HAL_USB_LOG
#define FS_HAL_USB_LOG 0
#endif

#define CAN_SPI        spi0
#define CAN_SPI_HZ     8000000 /* the MCP2515 takes up to 10 MHz */
#define CAN_PIN_MISO   4
#define CAN_PIN_CS     5
#define CAN_PIN_SCK    6
#define CAN_PIN_MOSI   7
#define CAN_SERVICE_US 100000u /* receive overflows and bus-off recovery: checked at 10 Hz */

static uint8_t reset_cause;
static uint64_t idle_us;

/* Linker symbols (memmap_default.ld): the stack's top, the bottom of its 2 KB reservation, and its limit */
extern char __StackTop[], __StackBottom[], __StackLimit[];
#define STACK_PAINT 0x5AC4C0DEu

static void stack_paint(void)
{
    uint32_t *p     = (uint32_t *)(void *)__StackLimit;
    uint32_t *frame = (uint32_t *)__builtin_frame_address(0);

    for (; p < frame - 64; p++) /* up to 256 bytes below this function's frame, which is in use */
    {
        *p = STACK_PAINT;
    }
}

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

#if FS_HAL_USB_LOG
#define USB_LOG_RING 2048
static char     usb_log[USB_LOG_RING];
static unsigned usb_log_head, usb_log_tail;

static void usb_log_put(const char *s)
{
    for (; *s; s++)
    {
        unsigned next = (usb_log_head + 1) % USB_LOG_RING;
        if (next == usb_log_tail)
        {
            return; /* full: the oldest lines, from start-up, are kept */
        }
        usb_log[usb_log_head] = *s;
        usb_log_head          = next;
    }
}

static void usb_log_drain(void)
{
    if (!stdio_usb_connected())
    {
        return; /* no terminal has the port open */
    }
    while (usb_log_tail != usb_log_head)
    {
        unsigned end = usb_log_head > usb_log_tail ? usb_log_head : USB_LOG_RING;
        stdio_usb.out_chars(&usb_log[usb_log_tail], (int)(end - usb_log_tail));
        usb_log_tail = end % USB_LOG_RING;
    }
}
#else
static void usb_log_put(const char *s)
{
    (void)s;
}

static void usb_log_drain(void)
{
}
#endif

/* Shown by picotool info: which board this is, and what its pins do */
bi_decl(bi_program_description("HITL FlatSat: " USBD_PRODUCT));
bi_decl(bi_2pins_with_func(LOG_UART_TX, LOG_UART_RX, GPIO_FUNC_UART));
bi_decl(bi_3pins_with_func(CAN_PIN_MISO, CAN_PIN_MOSI, CAN_PIN_SCK, GPIO_FUNC_SPI));
bi_decl(bi_1pin_with_name(CAN_PIN_CS, "CAN board CS"));

static mcp2515_t can;
static int       can_fitted;
static uint64_t  can_next_service;

static int can_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    gpio_put(CAN_PIN_CS, 0);
    if (rx != NULL)
    {
        spi_write_read_blocking(CAN_SPI, tx, rx, len);
    }
    else
    {
        spi_write_blocking(CAN_SPI, tx, len);
    }
    gpio_put(CAN_PIN_CS, 1);
    return 0;
}

static void can_start(void)
{
    int rc;

    spi_init(CAN_SPI, CAN_SPI_HZ);
    gpio_set_function(CAN_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(CAN_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(CAN_PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(CAN_PIN_MISO); /* without the board every byte reads 0xFF, and the driver reports no controller */
    gpio_init(CAN_PIN_CS);
    gpio_set_dir(CAN_PIN_CS, GPIO_OUT);
    gpio_put(CAN_PIN_CS, 1);

    can.xfer   = can_xfer;
    can.ctx    = NULL;
    rc         = mcp2515_init(&can);
    can_fitted = rc == MCP2515_OK;
    if (can_fitted)
    {
        fs_hal_log("CAN board: MCP2515 at 500 kbit/s");
    }
    else
    {
        fs_hal_log("no CAN board (MCP2515 error %d)", rc);
    }
}

/* Clears receive overflows and recovers from bus-off (mcp2515_service), logging each */
static void can_service(void)
{
    uint32_t overflows = can.rx_overflows;
    uint32_t bus_off   = can.bus_off;
    uint64_t now       = time_us_64();

    if (now < can_next_service)
    {
        return;
    }
    can_next_service = now + CAN_SERVICE_US;
    mcp2515_service(&can);
    if (can.rx_overflows != overflows)
    {
        fs_hal_log("CAN: receive overflow, frames lost (%lu so far)", (unsigned long)can.rx_overflows);
    }
    if (can.bus_off != bus_off)
    {
        fs_hal_log("CAN: bus-off, controller restarted (%lu so far)", (unsigned long)can.bus_off);
    }
}

int fs_hal_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    stack_paint();
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
    can_start();

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
    uint64_t start;

    log_drain();
    usb_log_drain();
    start = time_us_64();
    sleep_us(us);
    idle_us += time_us_64() - start;
}

/* ---- Resources ---- */

uint64_t fs_hal_idle_us(void)
{
    return idle_us;
}

int fs_hal_stack_used(void)
{
    const uint32_t *p = (const uint32_t *)(void *)__StackLimit;

    while ((const char *)p < __StackTop && *p == STACK_PAINT)
    {
        p++;
    }
    return (int)(__StackTop - (const char *)p);
}

int fs_hal_stack_reserved(void)
{
    return (int)(__StackTop - __StackBottom);
}

int fs_hal_stack_available(void)
{
    return (int)(__StackTop - __StackLimit);
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

/* ---- CAN: the CAN board, or over the umbilical without it ---- */

int fs_hal_can_present(void)
{
    return can_fitted || fs_umb_canbus_present();
}

int fs_hal_can_send(const fs_can_frame_t *frame)
{
    if (can_fitted)
    {
        return mcp2515_send(&can, frame) == MCP2515_OK ? 0 : -1;
    }
    return fs_umb_canbus_send(frame) == FS_UMB_OK ? 0 : -1;
}

int fs_hal_can_recv(fs_can_frame_t *frame)
{
    if (can_fitted)
    {
        can_service();
        return mcp2515_recv(&can, frame);
    }
    return fs_umb_canbus_recv(frame);
}

void fs_hal_can_error_counters(uint8_t *tec, uint8_t *rec)
{
    *tec = 0;
    *rec = 0;
    if (can_fitted)
    {
        mcp2515_error_counters(&can, tec, rec);
    }
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

    n = snprintf(line, sizeof(line), "[%6lu.%03lu] " FS_HAL_NAME ": ", (unsigned long)(ms / 1000u),
                 (unsigned long)(ms % 1000u));
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    log_put(line);
    log_put("\r\n");
    log_drain();
    usb_log_put(line);
    usb_log_put("\r\n");
    usb_log_drain();
    if (!in_log && fs_umb_link_up())
    {
        in_log = 1;
        fs_umb_log("%s", line + n);
        in_log = 0;
    }
}

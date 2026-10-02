/*
** Umbilical client: the flight computer's side of the hil_link protocol (ICD IF-01, section 8)
**
** Gives OBC software access to the NOS3 device sims (UART, I2C, SPI and CAN transactions through the
** HIL bridge), the magnetorquer sim, umbilical telemetry and telecommands, the simulated RF link and
** NOS3 simulation time. Frames the bridge sends unprompted are delivered through the handlers.
**
** Transactions block until the bridge responds or the timeout expires; frames that arrive meanwhile
** are still dispatched. Handlers run inside fs_umb_poll() or a waiting transaction, so they must not
** start transactions themselves: a nested call returns FS_UMB_BUSY.
*/
#ifndef FS_UMBILICAL_H
#define FS_UMBILICAL_H

#include <stddef.h>
#include <stdint.h>

#include "fs_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Results of transactions: hil_status_t values (>= 0) from the bridge, or these local errors */
#define FS_UMB_OK        0
#define FS_UMB_TIMEOUT  (-1)
#define FS_UMB_BUSY     (-2) /* called from inside a handler */
#define FS_UMB_TOO_LONG (-3)
#define FS_UMB_IO       (-4)

/* A link is "up" if any frame arrived within this time */
#define FS_UMB_LINK_TIMEOUT_US 3000000u

typedef struct
{
    void (*on_command)(const uint8_t *pkt, size_t len);  /* CI_PKT: umbilical telecommand */
    void (*on_rf_frame)(const uint8_t *frame, size_t len); /* RF_RX: frame from the link emulator */
    void (*on_time)(fs_time_t sim_time);                   /* TIME: NOS3 simulation time */
} fs_umb_handlers_t;

typedef struct
{
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t decode_errors;
    uint32_t timeouts;
    uint32_t bus_errors;
    uint32_t uart_overflows;
} fs_umb_stats_t;

void fs_umb_init(const fs_umb_handlers_t *handlers);

/* Reads and dispatches everything the bridge has sent; call it from the main loop */
void fs_umb_poll(void);

int fs_umb_link_up(void);
/* How long the link has been up without interruption, 0 if it is down */
uint64_t fs_umb_link_up_for_us(void);
const fs_umb_stats_t *fs_umb_stats(void);

/* ---- NOS3 device sims ---- */

/*
** Declare the buses the OBC uses. The bridge opens a NOS Engine bus on first use, which takes about
** 50 ms and would count against that transaction's timeout; declared buses are opened as soon as the
** link comes up (and again after every link loss). Transactions work on undeclared buses too.
*/
int fs_umb_i2c_open(uint8_t bus);
int fs_umb_spi_open(uint8_t bus, uint8_t cs);
int fs_umb_can_open(uint8_t bus);

int fs_umb_i2c(uint8_t bus, uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
               uint32_t timeout_ms);
int fs_umb_spi(uint8_t bus, uint8_t cs, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
               uint32_t timeout_ms);
int fs_umb_can(uint8_t bus, uint32_t id, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
               uint32_t timeout_ms);

/* Opens usart_<bus> and starts buffering what the sim sends on it */
int    fs_umb_uart_open(uint8_t bus);
int    fs_umb_uart_write(uint8_t bus, const uint8_t *data, size_t len);
size_t fs_umb_uart_available(uint8_t bus);
size_t fs_umb_uart_read(uint8_t bus, uint8_t *buf, size_t max);
void   fs_umb_uart_flush(uint8_t bus);

/* Magnetorquer duty in 0.01 % (-10000..10000) */
int fs_umb_trq(uint8_t torquer, int16_t duty);

/* ---- Packets and frames ---- */

int fs_umb_heartbeat(void);                              /* HEARTBEAT: send at least 1 Hz (ICD 8.1) */
int fs_umb_send_tm(const uint8_t *pkt, size_t len);       /* TO_PKT */
int fs_umb_send_rf(const uint8_t *frame, size_t len);     /* RF_TX (software-in-the-loop only) */
void fs_umb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* LOG */

#ifdef __cplusplus
}
#endif

#endif /* FS_UMBILICAL_H */

/*
** FlatSat hardware abstraction layer: the functions every node needs.
**
** Each platform provides one implementation:
**   hal/linux  software-in-the-loop: pty for the umbilical, SocketCAN (vcan) for CAN
**   hal/pico   hardware: USB CDC for the umbilical, MCP2515 over SPI for CAN
**
** Node-specific hardware (radio, power monitors, wheel motor, load switches, display) gets its own
** device-level interface in later headers, so the Linux build can model exactly the parts that are
** physical on the FlatSat.
**
** Conventions: functions returning int return >= 0 on success and < 0 on error unless stated.
** Nothing here blocks except fs_hal_sleep_us().
*/
#ifndef FS_HAL_H
#define FS_HAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle ---- */

/* Linux reads --umb, --can and similar options from argv; the Pico ignores both arguments */
int fs_hal_init(int argc, char **argv);

/* ---- Time ---- */

/* Monotonic microseconds since boot; never goes backwards */
uint64_t fs_hal_time_us(void);
void     fs_hal_sleep_us(uint32_t us);

/* ---- Umbilical serial link (hil_link frames, ICD IF-01) ---- */

int fs_hal_umb_write(const uint8_t *data, size_t len);
/* Copies up to max received bytes into buf; returns the count, 0 if nothing is waiting */
int fs_hal_umb_read(uint8_t *buf, size_t max);

/* ---- CAN bus (ICD IF-03) ---- */

typedef struct
{
    uint16_t id; /* 11-bit identifier */
    uint8_t  dlc;
    uint8_t  data[8];
} fs_can_frame_t;

int fs_hal_can_send(const fs_can_frame_t *frame);
/* Returns 1 and fills frame if one was waiting, 0 if none, < 0 on error */
int fs_hal_can_recv(fs_can_frame_t *frame);
/* Controller error counters (TEC/REC); the Linux virtual bus reports zeros */
void fs_hal_can_error_counters(uint8_t *tec, uint8_t *rec);

/* ---- System ---- */

/* flatsat_reset_cause_t value for the most recent reset */
uint8_t fs_hal_reset_cause(void);
void    fs_hal_reboot(void);
void    fs_hal_watchdog_kick(void);

/* Debug log line: stdout on Linux, the umbilical LOG frame or a UART on the Pico */
void fs_hal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* FS_HAL_H */

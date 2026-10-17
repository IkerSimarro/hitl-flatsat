/*
** INA219 current and voltage monitor (the EPS node's three rail monitors, ICD 6.3)
**
** Portable: the chip is reached through I2C callbacks, so the same code runs on the Pico (I2C0) and in the unit
** tests against a register-level model (tests/fake_ina219.c).
**
** Set-up for the breakout's 0.1 ohm shunt: 16 V bus range, shunt range +-80 mV (up to 800 mA, four times the
** wheel motor's peak), 12-bit conversions averaged over 8 samples (4.26 ms each: the motor's PWM ripple averages
** out), continuous shunt and bus conversion. Calibration for a current LSB of 0.1 mA: 0.04096 / (0.1 mA x 0.1
** ohm) = 4096.
*/
#ifndef INA219_H
#define INA219_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* addr is the 7-bit I2C address. Return 0 on success, < 0 if the device didn't acknowledge. */
    int (*write)(void *ctx, uint8_t addr, const uint8_t *data, size_t len);
    int (*read)(void *ctx, uint8_t addr, uint8_t *data, size_t len);
    void   *ctx;
    uint8_t addr;
} ina219_t;

#define INA219_OK        0
#define INA219_ERR_I2C   (-1) /* no acknowledge: not fitted, wrong address, or a bus fault */
#define INA219_ERR_CHIP  (-2) /* the configuration didn't read back */
#define INA219_ERR_RANGE (-3) /* the measurement overflowed (current beyond 800 mA) */

#define INA219_REG_CONFIG  0x00
#define INA219_REG_SHUNT   0x01
#define INA219_REG_BUS     0x02
#define INA219_REG_POWER   0x03
#define INA219_REG_CURRENT 0x04
#define INA219_REG_CAL     0x05

#define INA219_CONFIG      0x0DDF /* 16 V, PGA /2 (+-80 mV), 12-bit x8 bus and shunt, continuous */
#define INA219_CAL         4096   /* current LSB 0.1 mA with 0.1 ohm */

int ina219_init(ina219_t *d);
/* Bus voltage in mV and current in mA (positive: drawn by the load) */
int ina219_read(ina219_t *d, uint16_t *bus_mv, int16_t *current_ma);

#ifdef __cplusplus
}
#endif

#endif /* INA219_H */

/*
** Register-level model of INA219 monitors on one I2C bus (tests/test_drivers.c): configuration and calibration
** registers, and shunt, bus and current registers computed from a true voltage and current set by the test, for
** a 0.1 ohm shunt as on the FlatSat's breakouts.
*/
#ifndef FAKE_INA219_H
#define FAKE_INA219_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint8_t  addr;
    int      present;
    int      ignore_writes; /* a different chip at the address */
    uint16_t regs[6];
    uint8_t  ptr;
    double   true_mv, true_ma;
} fake_ina219_t;

extern fake_ina219_t fake_ina219[3];

void fake_ina219_reset(void);
int  fake_ina219_write(void *ctx, uint8_t addr, const uint8_t *data, size_t len);
int  fake_ina219_read(void *ctx, uint8_t addr, uint8_t *data, size_t len);

#endif /* FAKE_INA219_H */

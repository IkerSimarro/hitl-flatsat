/*
** EPS node: power hardware (three INA219 monitors on I2C, TP4056 status pins, two switch outputs)
**
** Rails (ICD 6.3): 0 battery out (total), 1 OBC + radio, 2 ADCS node including the wheel motor.
** Switches (ICD DD-07): 0 ADCS node RUN pin, 1 DRV8833 nSLEEP; on = released / enabled.
** Pico: I2C to the INA219s, GPIO for the rest. Linux (SIL): hal/linux/dev_power_linux.c.
*/
#ifndef DEV_POWER_H
#define DEV_POWER_H

#include <stdint.h>

#define DEV_POWER_NUM_RAILS    3
#define DEV_POWER_NUM_SWITCHES 2
#define DEV_POWER_RAIL_BATTERY 0
#define DEV_POWER_RAIL_OBC     1
#define DEV_POWER_RAIL_ADCS    2
#define DEV_POWER_SW_ADCS_RUN  0
#define DEV_POWER_SW_MOTOR     1

int  dev_power_init(void);
/* INA219 bus voltage (mV) and current (mA, positive = drawn by the load); 0 or < 0 on I2C error */
int  dev_power_read_rail(uint8_t rail, uint16_t *mv, int16_t *ma);
void dev_power_set_switch(uint8_t sw, int on);
int  dev_power_get_switch(uint8_t sw);
/* flatsat_charge_state_t from the TP4056 CHRG/STDBY pins */
uint8_t dev_power_charge_state(void);

#endif /* DEV_POWER_H */

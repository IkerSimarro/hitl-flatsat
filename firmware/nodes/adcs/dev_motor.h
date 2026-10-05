/*
** ADCS node: wheel motor hardware (DRV8833 driver, N20 motor with quadrature Hall encoder)
**
** Pico: PWM on the DRV8833 inputs, PIO quadrature counter, nSLEEP also wired to an input.
** Linux (SIL): the SIL harness and plant model (hal/linux/dev_motor_linux.c).
*/
#ifndef DEV_MOTOR_H
#define DEV_MOTOR_H

#include <stdint.h>

#define DEV_MOTOR_COUNTS_PER_REV 840 /* 7 pulses x 30:1 gearbox x 4 edges, at the output shaft */

int     dev_motor_init(void);
/* -1..1; the sign selects the direction */
void    dev_motor_set_duty(float duty);
/* Free-running quadrature count (wraps) */
int32_t dev_motor_encoder_count(void);
/* The EPS node can disable the driver (nSLEEP, ICD 6.3); 1 = enabled */
int     dev_motor_driver_enabled(void);

#endif /* DEV_MOTOR_H */

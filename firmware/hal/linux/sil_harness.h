/*
** SIL harness: the FlatSat's wiring between the node processes and the plant model in software-in-the-loop
**
** On the FlatSat these are wires, pins and sensors. In SIL they live in POSIX shared memory
** (/flatsat-harness): each node's Linux device layer writes its outputs (PWM duty, switch lines) and reads
** its inputs (encoder count, INA219 readings), and the plant model (firmware/sil_plant) integrates the
** physics in between: wheel motor and encoder, battery, charger and rail currents. Physics keeps running
** whatever the nodes do, e.g. the wheel coasts while the ADCS node is held in reset.
** Each field has a single writer, so plain loads and stores are enough for a simulation.
*/
#ifndef SIL_HARNESS_H
#define SIL_HARNESS_H

#include <stdint.h>

#define SIL_NUM_RAILS 3

typedef struct
{
    uint32_t magic;

    /* ---- Node outputs ---- */
    volatile uint8_t adcs_run;     /* EPS: ADCS Pico RUN pin, 0 holds the ADCS node in reset */
    volatile uint8_t motor_nsleep; /* EPS: DRV8833 nSLEEP, 0 removes drive from the wheel motor */
    volatile float   motor_duty;   /* ADCS: PWM duty to the DRV8833, -1..1 (sign = direction) */
    volatile float   radio_ma;     /* OBC: LoRa radio current on top of the OBC's base load */

    /* ---- Environment (set by tests or the operator) ---- */
    volatile uint8_t usb_power; /* TP4056 charger input present */

    /* ---- Plant outputs ---- */
    volatile int32_t encoder_count;              /* wheel motor quadrature counts (840 per output rev) */
    volatile float   wheel_rpm;                  /* true wheel speed, for tests only */
    volatile float   motor_ma;                   /* true motor current */
    volatile float   rail_v[SIL_NUM_RAILS];      /* true rail voltages: 0 battery out, 1 OBC + radio, */
    volatile float   rail_ma[SIL_NUM_RAILS];     /*   2 ADCS node incl. motor; and currents (ICD 6.3) */
    volatile float   battery_v;                  /* terminal voltage */
    volatile float   battery_soc;                /* state of charge 0..1, for tests only */
    volatile uint8_t charger_chrg;               /* TP4056 CHRG pin active (charging) */
    volatile uint8_t charger_stdby;              /* TP4056 STDBY pin active (charge complete) */
    volatile uint32_t plant_steps;               /* increments every plant step: the plant is alive */
} sil_harness_t;

/* Maps the shared harness, creating and initialising it (lines released, idle) on first use */
sil_harness_t *sil_harness(void);

#endif /* SIL_HARNESS_H */

/*
** ADCS default parameters, shared by the OBC and the closed-loop unit test (docs/design/ADCS_DESIGN.md)
*/
#ifndef ADCS_PARAMS_H
#define ADCS_PARAMS_H

#define ADCS_PI 3.14159265f
#define ADCS_DEG (ADCS_PI / 180.0f)

/* ---- Spacecraft (nos3/cfg/InOut/SC_NOS3.txt) ---- */
#define ADCS_INERTIA_X    0.0067f   /* kg m^2 */
#define ADCS_INERTIA_Y    0.033f
#define ADCS_INERTIA_Z    0.033f
#define ADCS_MTB_MAX      1.42f     /* A m^2 per torquer: duty 1.0 */
#define ADCS_RW_TORQUE    0.001f    /* N m per wheel */
#define ADCS_RW_MOMENTUM  0.01082f  /* N m s per wheel */
#define ADCS_RW_INERTIA   1.72e-5f  /* kg m^2 wheel rotor */

/* ---- Control ---- */
#define ADCS_PERIOD_S     0.2f      /* control loop, 5 Hz */
#define ADCS_BDOT_GAIN    20.0f     /* A m^2 s: m = -k dB/dt / |B|. Chosen by a gain scan (ADCS_DESIGN.md 4.1, NCR-009):
                                       higher gains lock the spin onto the field line and stall */
#define ADCS_BDOT_FILTER  0.5f      /* weight of the newest field derivative */
#define ADCS_SUN_KP       0.04f     /* 1/s^2: wn = 0.2 rad/s */
#define ADCS_SUN_KD       0.28f     /* 1/s: zeta = 0.7 */
#define ADCS_SLEW_RATE    (2.0f * ADCS_DEG) /* rad/s */
#define ADCS_MM_GAIN      0.002f    /* 1/s: momentum management, ~500 s time constant */
#define ADCS_CSS_MIN      0.1f      /* minimum CSS sun vector magnitude: below it the Sun isn't trusted */

/* ---- Automatic mode transitions (ICD 5.1) ---- */
#define ADCS_TUMBLE_RATE      (2.5f * ADCS_DEG)  /* SAFE -> DETUMBLE above this body rate (NOS3's tip-off: 2.8)... */
#define ADCS_TUMBLE_TIME_S    10.0f              /*   ...for this long */
#define ADCS_DETUMBLED_RATE   (2.0f * ADCS_DEG)  /* DETUMBLE -> SUN_POINT below this rate for this long: B-dot */
#define ADCS_DETUMBLED_TIME_S 20.0f              /*   can stall above 1 deg/s (spin about the field line), the */
                                                 /*   wheels absorb 2 deg/s with 11 % of their capacity */
#define ADCS_LOST_RATE        (5.0f * ADCS_DEG)  /* SUN_POINT -> DETUMBLE above this rate... */
#define ADCS_LOST_TIME_S      10.0f              /*   ...for this long */
#define ADCS_POINTED_ERROR    (5.0f * ADCS_DEG)  /* sun pointing converged: error below this */
#define ADCS_POINTED_RATE     (0.2f * ADCS_DEG)  /*   and rate below this */

/* ---- Power (NOS3 EPS sim battery: 22.8 V empty to 25.2 V full, linear) ---- */
#define ADCS_BATT_EMPTY_V     22.8f
#define ADCS_BATT_FULL_V      25.2f
#define ADCS_LOW_SOC          0.25f /* -> LOW_POWER below this state of charge... */
#define ADCS_LOW_TIME_S       5.0f  /*   ...for this long */
#define ADCS_RECOVER_SOC      0.50f /* LOW_POWER -> SAFE above this... */
#define ADCS_RECOVER_TIME_S   10.0f /*   ...for this long */

#endif /* ADCS_PARAMS_H */

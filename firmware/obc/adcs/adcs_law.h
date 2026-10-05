/*
** ADCS control laws (see docs/design/ADCS_DESIGN.md)
**
** Pure functions on body-frame vectors, no device access, so they run unchanged in the OBC, the unit tests'
** closed-loop simulation and on the Pico. Single precision: the RP2350's FPU is single precision.
*/
#ifndef ADCS_LAW_H
#define ADCS_LAW_H

#include <stdint.h>

#define ADCS_NUM_CSS 6

/* ---- Sun vector from the six coarse sun sensors ---- */

/* CSS boresights: +X, -X, +Y, -Y, +Z, -Z (42 SC_NOS3.txt). Each output is the cosine of the Sun angle,
** clipped at zero, so opposite pairs give one component each. Returns 1 and a unit vector if the
** illumination is strong enough to trust (not in eclipse), else 0 */
int adcs_sun_from_css(const float illum[ADCS_NUM_CSS], float min_norm, float sun[3]);

/* ---- B-dot detumbling with the magnetorquers ---- */

typedef struct
{
    float   b_prev[3];
    float   bdot[3];     /* filtered field derivative, T/s */
    uint8_t have_prev;
} adcs_bdot_t;

void adcs_bdot_reset(adcs_bdot_t *s);

/* m = -gain * dB/dt / |B| (A m^2), scaled down as a vector so no axis exceeds m_max. The first call after a
** reset only records the field and commands zero. filter: weight of the newest derivative (0..1] */
void adcs_bdot_step(adcs_bdot_t *s, const float b[3], float dt, float gain, float filter, float m_max,
                    float m_out[3]);

/* ---- Sun pointing with the reaction wheels ---- */

typedef struct
{
    float inertia[3]; /* principal moments, kg m^2 */
    float axis[3];    /* body axis to point at the Sun (unit) */
    float kp;         /* inertia-normalised gains: wn^2 (1/s^2) */
    float kd;         /*   and 2 zeta wn (1/s) */
    float rate_max;   /* slew rate limit, rad/s */
    float torque_max; /* per wheel, N m */
} adcs_sun_cfg_t;

/* Body torque command. With a valid Sun vector: T = I kd (w_des - w), w_des = (kp/kd) theta along the
** eigenaxis from the pointing axis to the Sun, limited to rate_max; without one (eclipse): rate damping
** only, T = -I kd w. Scaled as a vector so no axis exceeds torque_max. Returns the pointing error (rad),
** or -1 without a Sun vector */
float adcs_sun_point(const adcs_sun_cfg_t *cfg, const float sun[3], int sun_valid, const float rate[3],
                     float torque_out[3]);

/* ---- Wheel momentum management with the magnetorquers ---- */

/* m = -k (B x h) / |B|^2: the magnetic torque m x B = -k h, less its component along B (which no dipole
** can produce), drains the wheel momentum h while the wheels hold the attitude */
void adcs_momentum_dump(const float h[3], const float b[3], float k, float m_max, float m_out[3]);

/* ---- Helpers ---- */

float adcs_norm(const float v[3]);
/* Scales v so that no component exceeds limit in magnitude; returns the scale factor applied (<= 1) */
float adcs_limit_vector(float v[3], float limit);

#endif /* ADCS_LAW_H */

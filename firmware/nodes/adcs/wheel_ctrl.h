/*
** Reaction wheel speed control: encoder speed estimate and a PI loop with feed-forward
**
** Speed is the encoder count difference over the control period, low-pass filtered (one count per 10 ms
** is about 7 rpm). The duty is a feed-forward term (setpoint over the expected no-load speed per unit
** duty) plus PI on the speed error, clamped to +-1. The feed-forward does most of the work, so the
** integrator only trims the remaining error: it integrates only within integral_band of the set point
** (otherwise it charges up during spin-up and overshoots) and holds while the output is saturated in the
** direction of the error (anti-windup).
*/
#ifndef WHEEL_CTRL_H
#define WHEEL_CTRL_H

#include <stdint.h>

typedef struct
{
    /* Configuration */
    float kp;             /* duty per rpm */
    float ki;             /* duty per rpm per second */
    float rpm_per_duty;   /* feed-forward: expected speed at duty 1 */
    float filter_alpha;   /* speed low-pass, 0..1 (1 = no filtering) */
    float integral_band;  /* rpm: integrate only when |error| is below this */
    int   counts_per_rev;

    /* State */
    float   integral;
    float   duty;
    float   speed_rpm;
    int     saturated;
    int32_t last_count;
    int     have_count;
} wheel_ctrl_t;

void wheel_ctrl_init(wheel_ctrl_t *c, float kp, float ki, float rpm_per_duty, float filter_alpha, float integral_band,
                     int counts_per_rev);

/* Updates the speed estimate from the encoder count accumulated over dt seconds; returns it in rpm */
float wheel_ctrl_update_speed(wheel_ctrl_t *c, int32_t count, float dt);

/* One control step towards setpoint_rpm; returns the duty to apply */
float wheel_ctrl_step(wheel_ctrl_t *c, float setpoint_rpm, float dt);

/* Zero the output and the integrator (wheel off, safe state) */
void wheel_ctrl_reset(wheel_ctrl_t *c);

#endif /* WHEEL_CTRL_H */

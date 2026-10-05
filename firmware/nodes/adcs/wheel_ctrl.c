/*
** Reaction wheel speed control (see wheel_ctrl.h)
*/
#include "wheel_ctrl.h"

void wheel_ctrl_init(wheel_ctrl_t *c, float kp, float ki, float rpm_per_duty, float filter_alpha, float integral_band,
                     int counts_per_rev)
{
    c->integral_band  = integral_band;
    c->kp             = kp;
    c->ki             = ki;
    c->rpm_per_duty   = rpm_per_duty;
    c->filter_alpha   = filter_alpha;
    c->counts_per_rev = counts_per_rev;
    c->have_count     = 0;
    c->speed_rpm      = 0.0f;
    wheel_ctrl_reset(c);
}

void wheel_ctrl_reset(wheel_ctrl_t *c)
{
    c->integral  = 0.0f;
    c->duty      = 0.0f;
    c->saturated = 0;
}

float wheel_ctrl_update_speed(wheel_ctrl_t *c, int32_t count, float dt)
{
    if (c->have_count && dt > 0.0f)
    {
        /* Unsigned subtraction handles counter wrap-around */
        int32_t delta = (int32_t)((uint32_t)count - (uint32_t)c->last_count);
        float   raw   = (float)delta / (float)c->counts_per_rev / dt * 60.0f;
        c->speed_rpm += c->filter_alpha * (raw - c->speed_rpm);
    }
    c->last_count = count;
    c->have_count = 1;
    return c->speed_rpm;
}

float wheel_ctrl_step(wheel_ctrl_t *c, float setpoint_rpm, float dt)
{
    float error = setpoint_rpm - c->speed_rpm;
    float u;

    /* Integrate near the set point only, and not further into a saturated output */
    if ((error < c->integral_band && error > -c->integral_band) &&
        !(c->saturated && ((c->duty > 0.0f && error > 0.0f) || (c->duty < 0.0f && error < 0.0f))))
    {
        c->integral += c->ki * error * dt;
    }

    u = setpoint_rpm / c->rpm_per_duty + c->kp * error + c->integral;
    c->saturated = 0;
    if (u > 1.0f)
    {
        u            = 1.0f;
        c->saturated = 1;
    }
    else if (u < -1.0f)
    {
        u            = -1.0f;
        c->saturated = 1;
    }
    c->duty = u;
    return u;
}

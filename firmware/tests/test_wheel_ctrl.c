/*
** Unit test for the reaction wheel speed controller, in closed loop with a first-order motor model
** (the same structure as the SIL plant: no-load speed proportional to duty, time constant tau)
*/
#include <math.h>
#include <stdio.h>

#include "wheel_ctrl.h"

static int failures;
static int checks;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        checks++;                                                  \
        if (!(cond))                                               \
        {                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

#define CPR 840
#define DT  0.01f

typedef struct
{
    double rpm;
    double revs;
    double rpm_per_duty; /* the real motor, which the controller only estimates */
    double tau;
} motor_t;

static int32_t motor_step(motor_t *m, float duty, double dt)
{
    m->rpm += (duty * m->rpm_per_duty - m->rpm) * dt / m->tau;
    m->revs += m->rpm / 60.0 * dt;
    return (int32_t)floor(m->revs * CPR);
}

/* Runs the loop for `seconds`, returns the final true speed; records the peak and settling time */
static double run(wheel_ctrl_t *c, motor_t *m, float setpoint, double seconds, double *peak, double *settle_s)
{
    int32_t count = (int32_t)floor(m->revs * CPR);
    double  t;
    double  last_outside = 0.0;

    *peak = 0.0;
    for (t = 0.0; t < seconds; t += DT)
    {
        float duty;
        wheel_ctrl_update_speed(c, count, DT);
        duty  = wheel_ctrl_step(c, setpoint, DT);
        count = motor_step(m, duty, DT);
        if (fabs(m->rpm) > fabs(*peak))
        {
            *peak = m->rpm;
        }
        if (fabs(m->rpm - setpoint) > 0.05 * fabs(setpoint) + 7.0)
        {
            last_outside = t;
        }
    }
    *settle_s = last_outside;
    return m->rpm;
}

int main(void)
{
    wheel_ctrl_t c;
    motor_t      m = {0.0, 0.0, 650.0, 0.25};
    double       peak;
    double       settle;
    double       final;
    int32_t      near_wrap;

    /* Same tuning as the ADCS node; feed-forward deliberately 1.5 % off the real motor */
    wheel_ctrl_init(&c, 0.0008f, 0.004f, 640.0f, 0.3f, 50.0f, CPR);

    /* Step 0 -> 300 rpm: settles within 5 % in under 1.5 s, overshoot below 10 % */
    final = run(&c, &m, 300.0f, 4.0, &peak, &settle);
    printf("  step to 300 rpm: final %.1f rpm, peak %.1f, settled after %.2f s\n", final, peak, settle);
    CHECK(fabs(final - 300.0) < 10.0);
    CHECK(settle < 1.5);
    CHECK(peak < 330.0);

    /* Reversal to -300 rpm */
    final = run(&c, &m, -300.0f, 4.0, &peak, &settle);
    printf("  reverse to -300 rpm: final %.1f rpm, settled after %.2f s\n", final, settle);
    CHECK(fabs(final + 300.0) < 10.0);
    CHECK(settle < 2.5);

    /* Unreachable set point (900 rpm, motor tops out near 650): output saturates, integrator doesn't wind up */
    run(&c, &m, 900.0f, 3.0, &peak, &settle);
    CHECK(c.saturated && c.duty == 1.0f);
    CHECK(c.integral < 0.5f);
    /* ...so coming back down to 200 rpm is quick */
    final = run(&c, &m, 200.0f, 3.0, &peak, &settle);
    printf("  from saturation back to 200 rpm: final %.1f rpm, settled after %.2f s\n", final, settle);
    CHECK(fabs(final - 200.0) < 10.0);
    CHECK(settle < 1.5);

    /* Speed estimate across the 32-bit counter wrap */
    wheel_ctrl_init(&c, 0.0008f, 0.004f, 640.0f, 1.0f, 50.0f, CPR);
    near_wrap = 2147483647 - 50;
    wheel_ctrl_update_speed(&c, near_wrap, DT);
    wheel_ctrl_update_speed(&c, (int32_t)((uint32_t)near_wrap + 70u), DT); /* +70 counts in 10 ms */
    CHECK(fabsf(c.speed_rpm - 70.0f / CPR / DT * 60.0f) < 0.5f);          /* 500 rpm */

    if (failures)
    {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("wheel controller: all %d checks passed\n", checks);
    return 0;
}

/*
** Attitude determination and control (docs/design/ADCS_DESIGN.md)
**
** 5 Hz step: reads the ADCS sensors, then by system mode
**   DETUMBLE   B-dot on the magnetorquers
**   SUN_POINT  +X to the Sun on the wheels (rate damping in eclipse), wheel momentum dumped by the torquers
**   TEST       ADCS_RW_MANUAL wheel speed loops; ADCS_TRQ_MANUAL acts directly (obc_cmd.c)
**   SAFE, LOW_POWER: actuators off
** 1 Hz: fault responses (always on) and automatic mode transitions (OBC_SET_AUTO_MODES), ICD 5.1
*/
#include <math.h>
#include <string.h>

#include "adcs_law.h"
#include "adcs_params.h"
#include "fs_hal.h"
#include "obc.h"

#define MANUAL_RW_BANDWIDTH 1.0f /* 1/s: TEST mode wheel speed loop */

static adcs_bdot_t bdot;
static uint64_t    last_step_us;

/* When each automatic transition's condition became true (0 = not true) */
static uint64_t since_tumbling;
static uint64_t since_detumbled;
static uint64_t since_rates_lost;
static uint64_t since_battery_low;
static uint64_t since_battery_ok;

static int sensors_ok(uint16_t bits)
{
    return (obc.sensor_valid & bits) == bits;
}

static float body_rate(void)
{
    return adcs_norm(obc.imu.rate);
}

/* ---- Actuators ---- */

static void set_torquers(const float m[3])
{
    int i;

    for (i = 0; i < DEV_NUM_TRQ; i++)
    {
        float duty = m[i] / ADCS_MTB_MAX;
        dev_trq_set((uint8_t)i, duty);
        obc.trq_duty[i] = (int16_t)lroundf(duty * 10000.0f);
    }
}

/* Body torque from the wheels: each wheel is driven with the opposite torque */
static void set_wheels(const float body_torque[3])
{
    int i;

    for (i = 0; i < DEV_NUM_RW; i++)
    {
        obc.adcs.rw_torque[i] = -body_torque[i];
        dev_rw_set_torque((uint8_t)i, obc.adcs.rw_torque[i]);
    }
}

static void stop_actuators(void)
{
    static const float zero[3] = {0.0f, 0.0f, 0.0f};

    set_torquers(zero);
    set_wheels(zero);
}

/* ---- Control step ---- */

static void detumble(float dt)
{
    float m[3] = {0.0f, 0.0f, 0.0f};

    if (sensors_ok(OBC_VALID_MAG))
    {
        adcs_bdot_step(&bdot, obc.mag.field, dt, obc.bdot_gain, ADCS_BDOT_FILTER, ADCS_MTB_MAX, m);
    }
    else
    {
        adcs_bdot_reset(&bdot); /* restart the derivative when the field comes back */
    }
    set_torquers(m);
}

static void sun_point(void)
{
    adcs_sun_cfg_t cfg = {{ADCS_INERTIA_X, ADCS_INERTIA_Y, ADCS_INERTIA_Z}, {1.0f, 0.0f, 0.0f}, obc.sun_kp,
                          obc.sun_kd, ADCS_SLEW_RATE, ADCS_RW_TORQUE};
    float t[3] = {0.0f, 0.0f, 0.0f};
    float m[3] = {0.0f, 0.0f, 0.0f};
    float h[3];
    int   i;
    int   was_converged = obc.adcs.converged;

    obc.adcs.pointing_error = -1.0f;
    if (sensors_ok(OBC_VALID_IMU | OBC_VALID_RW))
    {
        obc.adcs.pointing_error = adcs_sun_point(&cfg, obc.adcs.sun, obc.adcs.sun_valid, obc.imu.rate, t);
        if (sensors_ok(OBC_VALID_MAG))
        {
            for (i = 0; i < 3; i++)
            {
                h[i] = (float)obc.rw_momentum[i];
            }
            adcs_momentum_dump(h, obc.mag.field, ADCS_MM_GAIN, ADCS_MTB_MAX, m);
        }
    }
    set_wheels(t);
    set_torquers(m);

    obc.adcs.converged = (uint8_t)(obc.adcs.pointing_error >= 0.0f &&
                                   obc.adcs.pointing_error < ADCS_POINTED_ERROR && body_rate() < ADCS_POINTED_RATE);
    if (obc.adcs.converged && !was_converged)
    {
        obc_event(EVT_ADCS, FLATSAT_SEVERITY_INFO, "sun pointing converged: error %.1f deg, rate %.2f deg/s",
                  obc.adcs.pointing_error / ADCS_DEG, body_rate() / ADCS_DEG);
    }
}

static void manual_wheels(void)
{
    int i;

    for (i = 0; i < DEV_NUM_RW; i++)
    {
        if (obc.adcs.manual_rw[i] && sensors_ok(OBC_VALID_RW))
        {
            float speed  = (float)obc.rw_momentum[i] / ADCS_RW_INERTIA;
            float torque = ADCS_RW_INERTIA * MANUAL_RW_BANDWIDTH * (obc.adcs.manual_rw_speed[i] - speed);
            torque       = fmaxf(-ADCS_RW_TORQUE, fminf(ADCS_RW_TORQUE, torque));
            obc.adcs.rw_torque[i] = torque;
            dev_rw_set_torque((uint8_t)i, torque);
        }
    }
}

void obc_adcs_step(void)
{
    uint64_t now = fs_hal_time_us();
    float    dt  = last_step_us ? (float)(now - last_step_us) / 1e6f : ADCS_PERIOD_S;

    last_step_us = now;
    obc_sensors_acquire_adcs();
    obc.adcs.sun_valid =
        (uint8_t)(sensors_ok(OBC_VALID_CSS) && adcs_sun_from_css(obc.css.illum, ADCS_CSS_MIN, obc.adcs.sun));

    switch (obc.mode)
    {
        case FLATSAT_MODE_DETUMBLE:
            obc.adcs.adcs_mode = FLATSAT_ADCS_MODE_BDOT;
            detumble(dt);
            break;
        case FLATSAT_MODE_SUN_POINT:
            obc.adcs.adcs_mode = FLATSAT_ADCS_MODE_SUN_POINT;
            sun_point();
            break;
        case FLATSAT_MODE_TEST:
            obc.adcs.adcs_mode = FLATSAT_ADCS_MODE_MANUAL;
            manual_wheels();
            break;
        default:
            obc.adcs.adcs_mode = FLATSAT_ADCS_MODE_OFF;
            break;
    }
}

/* ---- Fault responses and automatic transitions ---- */

/* 1 once cond has held continuously for `seconds` */
static int held(uint64_t *since, int cond, float seconds, uint64_t now)
{
    if (!cond)
    {
        *since = 0;
        return 0;
    }
    if (*since == 0)
    {
        *since = now;
    }
    return now - *since >= (uint64_t)(seconds * 1e6f);
}

static void reset_timers(void)
{
    since_tumbling = since_detumbled = since_rates_lost = since_battery_low = since_battery_ok = 0;
}

static void fault_to_safe(const char *what)
{
    obc_event(EVT_ADCS, FLATSAT_SEVERITY_ERROR, "%s failed in %s: SAFE", what, obc_mode_name(obc.mode));
    obc_mode_request(FLATSAT_MODE_SAFE, FLATSAT_MODE_REASON_FAULT);
}

void obc_adcs_auto(void)
{
    uint64_t now      = fs_hal_time_us();
    int      rates_ok = sensors_ok(OBC_VALID_IMU);
    float    rate     = body_rate();
    int      attitude = obc.mode == FLATSAT_MODE_DETUMBLE || obc.mode == FLATSAT_MODE_SUN_POINT;
    int      eps_ok   = sensors_ok(OBC_VALID_EPS);
    float    soc      = (obc.eps.batt_v - ADCS_BATT_EMPTY_V) / (ADCS_BATT_FULL_V - ADCS_BATT_EMPTY_V);

    /* Fault responses, always active: a declared sensor failure stops the control law that needs it */
    if (obc.mode == FLATSAT_MODE_DETUMBLE && (obc.sensor_failed & OBC_VALID_MAG))
    {
        fault_to_safe("magnetometer");
        return;
    }
    if (obc.mode == FLATSAT_MODE_SUN_POINT && (obc.sensor_failed & (OBC_VALID_IMU | OBC_VALID_RW)))
    {
        fault_to_safe(obc.sensor_failed & OBC_VALID_IMU ? "IMU" : "reaction wheels");
        return;
    }

    /* Low battery, always active except in TEST (the operator is in control) */
    if (held(&since_battery_low, eps_ok && soc < ADCS_LOW_SOC && (attitude || obc.mode == FLATSAT_MODE_SAFE),
             ADCS_LOW_TIME_S, now))
    {
        obc_event(EVT_ADCS, FLATSAT_SEVERITY_WARNING, "simulated battery at %.0f %%: LOW_POWER", soc * 100.0f);
        obc_mode_request(FLATSAT_MODE_LOW_POWER, FLATSAT_MODE_REASON_LOW_BATTERY);
        return;
    }

    if (!obc.adcs.auto_modes)
    {
        reset_timers();
        return;
    }
    switch (obc.mode)
    {
        case FLATSAT_MODE_SAFE:
            /* Detumbling needs the magnetometer; the IMU says when it's needed */
            if (held(&since_tumbling, rates_ok && sensors_ok(OBC_VALID_MAG) && rate > ADCS_TUMBLE_RATE,
                     ADCS_TUMBLE_TIME_S, now))
            {
                obc_event(EVT_ADCS, FLATSAT_SEVERITY_INFO, "tumbling at %.1f deg/s: DETUMBLE", rate / ADCS_DEG);
                obc_mode_request(FLATSAT_MODE_DETUMBLE, FLATSAT_MODE_REASON_AUTO_RATES_HIGH);
            }
            break;
        case FLATSAT_MODE_DETUMBLE:
            /* Sun pointing uses the wheels, so the ADCS node (the physical wheel) must be up too */
            if (held(&since_detumbled,
                     rates_ok && rate < ADCS_DETUMBLED_RATE && (obc_can_alive_mask() & (1u << FLATSAT_NODE_ADCS)),
                     ADCS_DETUMBLED_TIME_S, now))
            {
                obc_event(EVT_ADCS, FLATSAT_SEVERITY_INFO, "detumbled at %.2f deg/s: SUN_POINT", rate / ADCS_DEG);
                obc_mode_request(FLATSAT_MODE_SUN_POINT, FLATSAT_MODE_REASON_AUTO_CONVERGED);
            }
            break;
        case FLATSAT_MODE_SUN_POINT:
            if (held(&since_rates_lost, rates_ok && rate > ADCS_LOST_RATE, ADCS_LOST_TIME_S, now))
            {
                obc_event(EVT_ADCS, FLATSAT_SEVERITY_WARNING, "rate %.1f deg/s in sun pointing: DETUMBLE",
                          rate / ADCS_DEG);
                obc_mode_request(FLATSAT_MODE_DETUMBLE, FLATSAT_MODE_REASON_AUTO_RATES_HIGH);
            }
            break;
        case FLATSAT_MODE_LOW_POWER:
            if (held(&since_battery_ok, eps_ok && soc > ADCS_RECOVER_SOC, ADCS_RECOVER_TIME_S, now))
            {
                obc_event(EVT_ADCS, FLATSAT_SEVERITY_INFO, "simulated battery at %.0f %%: SAFE", soc * 100.0f);
                obc_mode_request(FLATSAT_MODE_SAFE, FLATSAT_MODE_REASON_BATTERY_RECOVERED);
            }
            break;
        default:
            break;
    }
}

/* ---- Interface ---- */

void obc_adcs_mode_changed(uint8_t from, uint8_t to)
{
    (void)from;
    (void)to;
    stop_actuators();
    adcs_bdot_reset(&bdot);
    memset(obc.adcs.manual_rw, 0, sizeof(obc.adcs.manual_rw));
    obc.adcs.converged      = 0;
    obc.adcs.pointing_error = -1.0f;
    reset_timers();
}

void obc_adcs_init(void)
{
    memset(&obc.adcs, 0, sizeof(obc.adcs));
    obc.adcs.auto_modes     = 1;
    obc.adcs.pointing_error = -1.0f;
    obc.bdot_gain           = ADCS_BDOT_GAIN;
    obc.sun_kp              = ADCS_SUN_KP;
    obc.sun_kd              = ADCS_SUN_KD;
    adcs_bdot_reset(&bdot);
    reset_timers();
    last_step_us = 0;
}

/*
** ADCS control laws (see adcs_law.h)
*/
#include "adcs_law.h"

#include <math.h>
#include <string.h>

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross(const float a[3], const float b[3], float out[3])
{
    float r[3];

    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
    memcpy(out, r, sizeof(r));
}

float adcs_norm(const float v[3])
{
    return sqrtf(dot(v, v));
}

float adcs_limit_vector(float v[3], float limit)
{
    float biggest = fmaxf(fabsf(v[0]), fmaxf(fabsf(v[1]), fabsf(v[2])));
    float scale   = 1.0f;
    int   i;

    if (biggest > limit && biggest > 0.0f)
    {
        scale = limit / biggest;
        for (i = 0; i < 3; i++)
        {
            v[i] *= scale;
        }
    }
    return scale;
}

/* ---- Sun vector ---- */

int adcs_sun_from_css(const float illum[ADCS_NUM_CSS], float min_norm, float sun[3])
{
    float s[3];
    float n;
    int   i;

    for (i = 0; i < 3; i++)
    {
        s[i] = illum[2 * i] - illum[2 * i + 1];
    }
    n = adcs_norm(s);
    if (n < min_norm)
    {
        sun[0] = sun[1] = sun[2] = 0.0f;
        return 0;
    }
    for (i = 0; i < 3; i++)
    {
        sun[i] = s[i] / n;
    }
    return 1;
}

/* ---- B-dot ---- */

void adcs_bdot_reset(adcs_bdot_t *s)
{
    memset(s, 0, sizeof(*s));
}

void adcs_bdot_step(adcs_bdot_t *s, const float b[3], float dt, float gain, float filter, float m_max,
                    float m_out[3])
{
    float bn = adcs_norm(b);
    int   i;

    if (!s->have_prev || dt <= 0.0f || bn <= 0.0f)
    {
        memcpy(s->b_prev, b, sizeof(s->b_prev));
        s->have_prev = 1;
        m_out[0] = m_out[1] = m_out[2] = 0.0f;
        return;
    }
    for (i = 0; i < 3; i++)
    {
        float raw = (b[i] - s->b_prev[i]) / dt;
        s->bdot[i] += filter * (raw - s->bdot[i]);
        m_out[i] = -gain * s->bdot[i] / bn;
    }
    memcpy(s->b_prev, b, sizeof(s->b_prev));
    adcs_limit_vector(m_out, m_max);
}

/* ---- Sun pointing ---- */

float adcs_sun_point(const adcs_sun_cfg_t *cfg, const float sun[3], int sun_valid, const float rate[3],
                     float torque_out[3])
{
    float w_des[3] = {0.0f, 0.0f, 0.0f};
    float err      = -1.0f;
    int   i;

    if (sun_valid)
    {
        float c = dot(cfg->axis, sun);
        float e[3];
        float en;

        c   = fmaxf(-1.0f, fminf(1.0f, c));
        err = acosf(c);
        cross(cfg->axis, sun, e); /* rotating about axis x sun turns the axis towards the Sun */
        en = adcs_norm(e);
        if (en < 1e-6f && c < 0.0f)
        {
            /* Sun exactly behind: any axis perpendicular to the pointing axis will do */
            float trial[3] = {cfg->axis[1], cfg->axis[2], cfg->axis[0]};
            cross(cfg->axis, trial, e);
            en = adcs_norm(e);
        }
        if (en > 1e-6f)
        {
            float speed = cfg->kp / cfg->kd * err;
            speed       = fminf(speed, cfg->rate_max);
            for (i = 0; i < 3; i++)
            {
                w_des[i] = speed * e[i] / en;
            }
        }
    }
    for (i = 0; i < 3; i++)
    {
        torque_out[i] = cfg->inertia[i] * cfg->kd * (w_des[i] - rate[i]);
    }
    adcs_limit_vector(torque_out, cfg->torque_max);
    return err;
}

/* ---- Momentum management ---- */

void adcs_momentum_dump(const float h[3], const float b[3], float k, float m_max, float m_out[3])
{
    float b2 = dot(b, b);
    int   i;

    if (b2 <= 0.0f)
    {
        m_out[0] = m_out[1] = m_out[2] = 0.0f;
        return;
    }
    cross(b, h, m_out);
    for (i = 0; i < 3; i++)
    {
        m_out[i] *= -k / b2;
    }
    adcs_limit_vector(m_out, m_max);
}

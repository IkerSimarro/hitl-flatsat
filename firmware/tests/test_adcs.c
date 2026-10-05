/*
** Closed-loop unit test of the ADCS control laws against a rigid-body simulation: the NOS3 spacecraft's
** inertia, three wheels and three magnetorquers with their limits, a geomagnetic field that turns at twice
** the orbit rate, cosine coarse sun sensors and sensor noise. The laws run at the OBC's 5 Hz; the dynamics
** at 200 Hz. This checks the design before it meets 42 (tests/system/test_adcs.py does that).
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adcs_law.h"
#include "adcs_params.h"

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

#define SIM_DT     0.005
#define ORBIT_RATE (2.0 * M_PI / 5550.0) /* rad/s, ~400 km */
#define DEG        (M_PI / 180.0)

/* ---- Simulation ---- */

typedef struct
{
    double c[3][3]; /* body-from-inertial direction cosine matrix */
    double w[3];    /* body rate, rad/s */
    double h[3];    /* wheel momentum, N m s (wheels on the body axes) */
    double t;
    int    eclipse;
    /* commands held between control steps */
    double m[3];  /* dipole, A m^2 */
    double tw[3]; /* torque on the wheels, N m */
} sim_t;

static const double inertia[3] = {ADCS_INERTIA_X, ADCS_INERTIA_Y, ADCS_INERTIA_Z};
static const double sun_inertial[3] = {0.267261, 0.801784, -0.534522}; /* (1, 3, -2) normalised */

static unsigned rng = 12345u;

static double noise(double sigma) /* uniform with the given standard deviation */
{
    rng = rng * 1103515245u + 12345u;
    return ((double)(rng >> 8) / 16777216.0 - 0.5) * sigma * 3.4641;
}

static void field_inertial(double t, double b[3])
{
    double a   = 2.0 * ORBIT_RATE * t;
    double mag = 30e-6 * (1.0 + 0.3 * cos(a));
    b[0]       = mag * cos(a);
    b[1]       = mag * 0.3;
    b[2]       = mag * sin(a);
}

static void to_body(const sim_t *s, const double v[3], double out[3])
{
    int i;
    for (i = 0; i < 3; i++)
    {
        out[i] = s->c[i][0] * v[0] + s->c[i][1] * v[1] + s->c[i][2] * v[2];
    }
}

static void sim_init(sim_t *s, const double w_deg[3])
{
    int i;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < 3; i++)
    {
        s->c[i][i] = 1.0;
        s->w[i]    = w_deg[i] * DEG;
    }
}

/* Rotates the attitude by -w dt (an inertially fixed vector turns by -w in the body frame) */
static void rotate(sim_t *s, double dt)
{
    double wn = sqrt(s->w[0] * s->w[0] + s->w[1] * s->w[1] + s->w[2] * s->w[2]);
    double k[3], r[3][3], c[3][3], th, ct, st;
    int    i, j, n;

    if (wn < 1e-12)
    {
        return;
    }
    th = -wn * dt;
    for (i = 0; i < 3; i++)
    {
        k[i] = s->w[i] / wn;
    }
    ct = cos(th);
    st = sin(th);
    /* Rodrigues: R = I cos + (1 - cos) k k^T + sin [k x] */
    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            r[i][j] = (i == j ? ct : 0.0) + (1.0 - ct) * k[i] * k[j];
        }
    }
    r[0][1] -= st * k[2];
    r[0][2] += st * k[1];
    r[1][0] += st * k[2];
    r[1][2] -= st * k[0];
    r[2][0] -= st * k[1];
    r[2][1] += st * k[0];
    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            c[i][j] = 0.0;
            for (n = 0; n < 3; n++)
            {
                c[i][j] += r[i][n] * s->c[n][j];
            }
        }
    }
    memcpy(s->c, c, sizeof(c));
}

static void sim_step(sim_t *s)
{
    double bi[3], b[3], tq[3], hb[3], gyro[3];
    int    i;

    field_inertial(s->t, bi);
    to_body(s, bi, b);
    for (i = 0; i < 3; i++)
    {
        /* Wheels stop accepting torque at their momentum limit */
        if (fabs(s->h[i]) >= ADCS_RW_MOMENTUM && s->tw[i] * s->h[i] > 0.0)
        {
            s->tw[i] = 0.0;
        }
        hb[i] = inertia[i] * s->w[i] + s->h[i];
    }
    /* I dw/dt = m x B - tw - w x (I w + h) */
    tq[0] = s->m[1] * b[2] - s->m[2] * b[1] - s->tw[0] - (s->w[1] * hb[2] - s->w[2] * hb[1]);
    tq[1] = s->m[2] * b[0] - s->m[0] * b[2] - s->tw[1] - (s->w[2] * hb[0] - s->w[0] * hb[2]);
    tq[2] = s->m[0] * b[1] - s->m[1] * b[0] - s->tw[2] - (s->w[0] * hb[1] - s->w[1] * hb[0]);
    for (i = 0; i < 3; i++)
    {
        s->w[i] += tq[i] / inertia[i] * SIM_DT;
        s->h[i] += s->tw[i] * SIM_DT;
    }
    rotate(s, SIM_DT);
    s->t += SIM_DT;
    (void)gyro;
}

/* ---- Flight software stand-in: sensors, the control laws, actuators ---- */

typedef enum
{
    CTRL_BDOT,
    CTRL_SUN
} ctrl_t;

typedef struct
{
    adcs_bdot_t    bdot;
    adcs_sun_cfg_t sun;
    int            mm;   /* momentum management on */
    float          err;  /* last pointing error, rad (-1 without Sun) */
    double         m_peak;
    double         tw_peak;
} fsw_t;

static void fsw_init(fsw_t *f)
{
    adcs_sun_cfg_t cfg = {{ADCS_INERTIA_X, ADCS_INERTIA_Y, ADCS_INERTIA_Z}, {1.0f, 0.0f, 0.0f}, ADCS_SUN_KP,
                          ADCS_SUN_KD, ADCS_SLEW_RATE, ADCS_RW_TORQUE};
    memset(f, 0, sizeof(*f));
    adcs_bdot_reset(&f->bdot);
    f->sun = cfg;
    f->mm  = 1;
}

static void fsw_step(fsw_t *f, sim_t *s, ctrl_t mode)
{
    static const double normals[ADCS_NUM_CSS][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    double bi[3], bb[3], sb[3];
    float  b[3], rate[3], illum[ADCS_NUM_CSS], sun[3], m[3], t[3], h[3];
    int    i, sun_valid;

    field_inertial(s->t, bi);
    to_body(s, bi, bb);
    to_body(s, sun_inertial, sb);
    for (i = 0; i < 3; i++)
    {
        b[i]    = (float)(round((bb[i] + noise(0.1e-9)) / 2e-9) * 2e-9); /* 42: 2 nT quantisation */
        rate[i] = (float)(s->w[i] + noise(1e-4));
        h[i]    = (float)s->h[i];
    }
    for (i = 0; i < ADCS_NUM_CSS; i++)
    {
        double c = normals[i][0] * sb[0] + normals[i][1] * sb[1] + normals[i][2] * sb[2];
        illum[i] = s->eclipse || c < 0.0 ? 0.0f : (float)(round(c / 0.001) * 0.001);
    }
    sun_valid = adcs_sun_from_css(illum, ADCS_CSS_MIN, sun);

    if (mode == CTRL_BDOT)
    {
        adcs_bdot_step(&f->bdot, b, ADCS_PERIOD_S, ADCS_BDOT_GAIN, ADCS_BDOT_FILTER, ADCS_MTB_MAX, m);
        t[0] = t[1] = t[2] = 0.0f;
    }
    else
    {
        f->err = adcs_sun_point(&f->sun, sun, sun_valid, rate, t);
        if (f->mm)
        {
            adcs_momentum_dump(h, b, ADCS_MM_GAIN, ADCS_MTB_MAX, m);
        }
        else
        {
            m[0] = m[1] = m[2] = 0.0f;
        }
    }
    for (i = 0; i < 3; i++)
    {
        s->m[i]  = m[i];
        s->tw[i] = -t[i]; /* the wheels take the reaction of the body torque */
        f->m_peak  = fmax(f->m_peak, fabs(m[i]));
        f->tw_peak = fmax(f->tw_peak, fabs(t[i]));
    }
}

static double rate_deg(const sim_t *s)
{
    return sqrt(s->w[0] * s->w[0] + s->w[1] * s->w[1] + s->w[2] * s->w[2]) / DEG;
}

static double sun_error_deg(const sim_t *s)
{
    double sb[3];
    to_body(s, sun_inertial, sb);
    return acos(fmax(-1.0, fmin(1.0, sb[0]))) / DEG;
}

static double h_norm(const sim_t *s)
{
    return sqrt(s->h[0] * s->h[0] + s->h[1] * s->h[1] + s->h[2] * s->h[2]);
}

/* Runs `seconds` of closed loop; returns the time at which cond() last became true and stayed true */
static void run(fsw_t *f, sim_t *s, ctrl_t mode, double seconds)
{
    int steps_per_ctrl = (int)(ADCS_PERIOD_S / SIM_DT + 0.5);
    int n              = (int)(seconds / SIM_DT);
    int k;

    for (k = 0; k < n; k++)
    {
        if (k % steps_per_ctrl == 0)
        {
            fsw_step(f, s, mode);
        }
        sim_step(s);
    }
}

/* ---- Tests ---- */

static void test_helpers(void)
{
    float illum[ADCS_NUM_CSS] = {0.6f, 0.0f, 0.0f, 0.48f, 0.64f, 0.0f};
    float dark[ADCS_NUM_CSS]  = {0.02f, 0.0f, 0.0f, 0.03f, 0.0f, 0.0f};
    float sun[3];
    float v[3] = {2.0f, -4.0f, 1.0f};

    CHECK(adcs_sun_from_css(illum, ADCS_CSS_MIN, sun) == 1);
    CHECK(fabsf(sun[0] - 0.6f) < 1e-5f && fabsf(sun[1] + 0.48f) < 1e-5f && fabsf(sun[2] - 0.64f) < 1e-5f);
    CHECK(adcs_sun_from_css(dark, ADCS_CSS_MIN, sun) == 0); /* eclipse or Earth albedo only */

    CHECK(fabsf(adcs_limit_vector(v, 1.0f) - 0.25f) < 1e-6f);
    CHECK(fabsf(v[0] - 0.5f) < 1e-6f && fabsf(v[1] + 1.0f) < 1e-6f && fabsf(v[2] - 0.25f) < 1e-6f); /* direction kept */
}

static void test_bdot_first_step(void)
{
    adcs_bdot_t s;
    float       b[3] = {20e-6f, -10e-6f, 5e-6f};
    float       m[3] = {1, 1, 1};

    adcs_bdot_reset(&s);
    adcs_bdot_step(&s, b, ADCS_PERIOD_S, ADCS_BDOT_GAIN, ADCS_BDOT_FILTER, ADCS_MTB_MAX, m);
    CHECK(m[0] == 0.0f && m[1] == 0.0f && m[2] == 0.0f); /* no derivative yet */
}

/* Detumbles from w0 with the given law; returns the time the rate first fell below `below` deg/s */
/* Detumbles from w0 (deg/s) with B-dot; returns the time the rate first fell below `below` deg/s */
static double detumble(const double w0[3], double seconds, double below, double *final, double *m_peak)
{
    sim_t  s;
    fsw_t  f;
    double t_done = -1.0;

    sim_init(&s, w0);
    fsw_init(&f);
    while (s.t < seconds - 1e-6)
    {
        run(&f, &s, CTRL_BDOT, 10.0);
        if (getenv("ADCS_TRACE") && (int)(s.t + 0.5) % 100 == 0)
        {
            printf("    t %5.0f  rate %.3f deg/s  (%.3f %.3f %.3f)\n", s.t, rate_deg(&s), s.w[0] / DEG, s.w[1] / DEG,
                   s.w[2] / DEG);
        }
        if (t_done < 0.0 && rate_deg(&s) < below)
        {
            t_done = s.t;
        }
    }
    *final  = rate_deg(&s);
    *m_peak = f.m_peak;
    return t_done;
}

static void test_detumble(void)
{
    /* Three tumbles: a gain that only suits one of them (NCR-009: 200 took 40 to 710 s) fails here */
    static const double tumbles[3][3] = {{3.0, -4.0, 5.0}, {2.0, -3.0, 4.0}, {-8.0, 6.0, 2.0}};
    int i;

    for (i = 0; i < 3; i++)
    {
        double w = sqrt(tumbles[i][0] * tumbles[i][0] + tumbles[i][1] * tumbles[i][1] + tumbles[i][2] * tumbles[i][2]);
        double t_handover, t_half, final, m_peak;

        /* To the hand-over rate, then on to the B-dot floor: about twice the orbit rate (0.13 deg/s), as the
         * field turns twice per orbit and the law follows it */
        t_handover = detumble(tumbles[i], 1500.0, ADCS_DETUMBLED_RATE / ADCS_DEG, &final, &m_peak);
        t_half     = detumble(tumbles[i], 1500.0, 0.5, &final, &m_peak);
        printf("  B-dot detumble from %.1f deg/s: below %.1f deg/s after %.0f s, below 0.5 after %.0f s, "
               "%.2f deg/s after 1500 s, peak dipole %.2f A m^2\n", w, ADCS_DETUMBLED_RATE / ADCS_DEG, t_handover,
               t_half, final, m_peak);
        CHECK(t_handover > 0.0 && t_handover < 120.0);
        CHECK(t_half > 0.0 && t_half < 600.0);
        CHECK(final < 0.4); /* oscillates with the field geometry, 0.14 to 0.28 */
        CHECK(m_peak <= ADCS_MTB_MAX + 1e-6);
    }
}

static void test_sun_pointing(void)
{
    const double w0[3] = {0.0, 0.0, 0.0};
    sim_t        s;
    fsw_t        f;
    double       t_done = -1.0;
    double       w_peak = 0.0;

    sim_init(&s, w0); /* +X starts 74.5 deg from the Sun */
    fsw_init(&f);
    f.mm = 0;
    while (s.t < 400.0)
    {
        run(&f, &s, CTRL_SUN, 1.0);
        w_peak = fmax(w_peak, rate_deg(&s));
        if (t_done < 0.0 && sun_error_deg(&s) < 2.0 && rate_deg(&s) < ADCS_POINTED_RATE / ADCS_DEG)
        {
            t_done = s.t;
        }
    }
    printf("  sun pointing from %.1f deg: within 2 deg after %.0f s, final %.2f deg, peak rate %.2f deg/s, "
           "peak wheel torque %.2f mN m\n", acos(sun_inertial[0]) / DEG, t_done, sun_error_deg(&s), w_peak,
           f.tw_peak * 1e3);
    CHECK(t_done > 0.0 && t_done < 150.0);
    CHECK(sun_error_deg(&s) < 1.0);
    CHECK(w_peak < ADCS_SLEW_RATE / ADCS_DEG * 1.25);
    CHECK(f.tw_peak <= ADCS_RW_TORQUE + 1e-9);
}

static void test_sun_behind(void)
{
    /* The Sun on -X (exactly behind the pointing axis): the law must still pick a direction */
    const double w0[3] = {0.0, 0.0, 0.0};
    sim_t        s;
    fsw_t        f;
    double       turn[3][3];
    int          i, j;

    sim_init(&s, w0);
    /* Attitude with body +X along -sun: rotate 180 deg about an axis perpendicular to the Sun line */
    memset(turn, 0, sizeof(turn));
    {
        double a[3] = {sun_inertial[1], -sun_inertial[0], 0.0}; /* perpendicular to sun_inertial */
        double an   = sqrt(a[0] * a[0] + a[1] * a[1]);
        double xb[3];
        /* body x = -sun, body y = a, body z = x cross y */
        for (i = 0; i < 3; i++)
        {
            xb[i] = -sun_inertial[i];
            a[i] /= an;
        }
        for (j = 0; j < 3; j++)
        {
            turn[0][j] = xb[j];
            turn[1][j] = a[j];
        }
        turn[2][0] = xb[1] * a[2] - xb[2] * a[1];
        turn[2][1] = xb[2] * a[0] - xb[0] * a[2];
        turn[2][2] = xb[0] * a[1] - xb[1] * a[0];
    }
    memcpy(s.c, turn, sizeof(turn));
    fsw_init(&f);
    f.mm = 0;
    printf("  sun pointing from %.1f deg (Sun behind):", sun_error_deg(&s));
    run(&f, &s, CTRL_SUN, 250.0);
    printf(" %.2f deg after 250 s\n", sun_error_deg(&s));
    CHECK(sun_error_deg(&s) < 2.0);
}

static void test_eclipse(void)
{
    const double w0[3] = {0.4, -0.3, 0.5};
    sim_t        s;
    fsw_t        f;

    sim_init(&s, w0);
    fsw_init(&f);
    f.mm      = 0;
    s.eclipse = 1;
    run(&f, &s, CTRL_SUN, 60.0);
    printf("  eclipse: rate damped from %.2f to %.3f deg/s in 60 s, no Sun vector (error %.0f)\n",
           sqrt(0.16 + 0.09 + 0.25), rate_deg(&s), f.err);
    CHECK(f.err < 0.0f);
    CHECK(rate_deg(&s) < 0.05);
    /* Out of eclipse: acquires the Sun */
    s.eclipse = 0;
    run(&f, &s, CTRL_SUN, 200.0);
    CHECK(sun_error_deg(&s) < 2.0);
}

static void test_momentum_management(void)
{
    const double w0[3] = {0.0, 0.0, 0.0};
    sim_t        s;
    fsw_t        f;
    double       h0;
    double       worst = 0.0;

    sim_init(&s, w0);
    fsw_init(&f);
    f.mm = 0;
    run(&f, &s, CTRL_SUN, 150.0); /* point first */
    s.h[0] = 0.004;
    s.h[1] = -0.003;
    s.h[2] = 0.002;
    h0     = h_norm(&s);
    f.mm   = 1;
    while (s.t < 150.0 + 3000.0)
    {
        run(&f, &s, CTRL_SUN, 5.0);
        worst = fmax(worst, sun_error_deg(&s));
    }
    printf("  momentum management: wheel momentum %.2f -> %.2f mN m s in 3000 s, pointing error stayed below "
           "%.2f deg\n", h0 * 1e3, h_norm(&s) * 1e3, worst);
    CHECK(h_norm(&s) < 0.3 * h0);
    CHECK(worst < ADCS_POINTED_ERROR / ADCS_DEG);
}

int main(void)
{
    test_helpers();
    test_bdot_first_step();
    test_detumble();
    test_sun_pointing();
    test_sun_behind();
    test_eclipse();
    test_momentum_management();

    if (failures)
    {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("ADCS control laws: all %d checks passed\n", checks);
    return 0;
}

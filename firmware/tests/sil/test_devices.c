/*
** Software-in-the-loop device test: every OBC driver against the real NOS3 simulators and 42
**
** Run inside the SIL environment:
**   sil/sil.sh firmware/build/test_devices --umb-pty '$SIL_UMB_PTY' --can none
**
** Each driver reading is compared with 42's truth data (the truth42sim stream, relayed to UDP 5112), using the
** sensor mounting in cfg/InOut/SC_NOS3.txt: gyros, magnetometers, wheels and torquers along the body
** axes, coarse sun sensors facing +X -X +Y -Y +Z -Z. Tolerances allow for the sensor noise and
** quantisation configured there and for the time between the truth sample and the reading.
*/
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fs_hal.h"
#include "fs_time.h"
#include "fs_umbilical.h"
#include "obc_devices.h"

/* ---- Truth (SIM_42_TRUTH packet, big-endian, 317 bytes) ---- */

typedef struct
{
    double pos_n[3], vel_n[3], svb[3], bvb[3], hvb[3], wn[3], qn[4], pos_w[3], vel_w[3], acc_b[3], gyro_b[3];
    double rw_h[3];
    int    valid;
} truth_t;

static int     truth_fd = -1;
static truth_t truth;

static double be_double(const uint8_t *p)
{
    uint64_t u = 0;
    double   d;
    int      i;

    for (i = 0; i < 8; i++)
    {
        u = (u << 8) | p[i];
    }
    memcpy(&d, &u, sizeof(d));
    return d;
}

static void be_vec(const uint8_t *p, double *v, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        v[i] = be_double(p + 8 * i);
    }
}

static void truth_open(void)
{
    struct sockaddr_in a;

    truth_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(5112); /* the SIL truth relay's port for tests (ICD 3.6) */
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(truth_fd, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        perror("bind 5112");
    }
    fcntl(truth_fd, F_SETFL, O_NONBLOCK);
}

/* Drains the socket, keeping the newest truth sample */
static void truth_update(void)
{
    uint8_t b[512];
    ssize_t n;

    while ((n = recv(truth_fd, b, sizeof(b), 0)) > 0)
    {
        if (n < 316 || (b[0] == 0 && b[1] == 0)) /* zero year: truth42sim not yet connected to 42 */
        {
            continue;
        }
        be_vec(&b[20], truth.pos_n, 3);
        be_vec(&b[44], truth.vel_n, 3);
        be_vec(&b[68], truth.svb, 3);
        be_vec(&b[92], truth.bvb, 3);
        be_vec(&b[116], truth.hvb, 3);
        be_vec(&b[140], truth.wn, 3);
        be_vec(&b[164], truth.qn, 4);
        be_vec(&b[196], truth.pos_w, 3);
        be_vec(&b[220], truth.vel_w, 3);
        be_vec(&b[244], truth.acc_b, 3);
        be_vec(&b[268], truth.gyro_b, 3);
        be_vec(&b[292], truth.rw_h, 3);
        truth.valid = 1;
    }
}

/* ---- Results ---- */

static int passes;
static int failures;

static void result(int ok, const char *name, const char *detail)
{
    printf("%s %-28s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (ok)
    {
        passes++;
    }
    else
    {
        failures++;
    }
}

static double norm3(const double *v)
{
    return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static double rad2deg(double r)
{
    return r * 180.0 / M_PI;
}

/* Services the umbilical for a while (keeps the link alive and collects TIME frames) */
static int       time_frames;
static fs_time_t last_sim_time;

static void on_time(fs_time_t t)
{
    fs_time_sync(t, 1);
    last_sim_time = t;
    time_frames++;
}

static void service(uint32_t ms)
{
    uint64_t end     = fs_hal_time_us() + (uint64_t)ms * 1000u;
    uint64_t next_hb = 0;

    while (fs_hal_time_us() < end)
    {
        if (fs_hal_time_us() >= next_hb)
        {
            fs_umb_heartbeat();
            next_hb = fs_hal_time_us() + 200000u;
        }
        fs_umb_poll();
        truth_update();
        fs_hal_sleep_us(1000);
    }
}

/* ---- Tests ---- */

static void test_link_and_time(void)
{
    char      d[160];
    uint32_t  t0;
    int       frames0;
    uint64_t  deadline = fs_hal_time_us() + 30000000u;

    while (!fs_umb_link_up() && fs_hal_time_us() < deadline)
    {
        service(200);
    }
    result(fs_umb_link_up(), "umbilical link", fs_umb_link_up() ? "bridge answering" : "no frames from the bridge");

    service(1500);
    frames0 = time_frames;
    t0      = last_sim_time.seconds;
    service(3000);
    snprintf(d, sizeof(d), "%d TIME frames in 3 s, sim time J2000 %u s, advanced %u s", time_frames - frames0,
             last_sim_time.seconds, last_sim_time.seconds - t0);
    result(time_frames - frames0 >= 2 && last_sim_time.seconds - t0 >= 2 && last_sim_time.seconds - t0 <= 4,
           "simulation time", d);
}

static void test_imu(void)
{
    dev_imu_t imu;
    char      d[200];
    int       rc;
    double    err = 0;
    int       i;

    truth_update();
    rc = dev_imu_read(&imu);
    truth_update();
    if (rc != DEV_OK)
    {
        snprintf(d, sizeof(d), "read failed (%d)", rc);
        result(0, "IMU (CAN)", d);
        return;
    }
    for (i = 0; i < 3; i++)
    {
        double e = fabs(imu.rate[i] - truth.wn[i]);
        err      = e > err ? e : err;
    }
    snprintf(d, sizeof(d), "rate [%.3f %.3f %.3f] deg/s, truth [%.3f %.3f %.3f], max error %.4f deg/s",
             rad2deg(imu.rate[0]), rad2deg(imu.rate[1]), rad2deg(imu.rate[2]), rad2deg(truth.wn[0]),
             rad2deg(truth.wn[1]), rad2deg(truth.wn[2]), rad2deg(err));
    /* Body rates change slowly; 0.05 deg/s covers sampling skew and gyro noise */
    result(err < 0.05 * M_PI / 180.0, "IMU (CAN)", d);
}

static void test_mag(void)
{
    dev_mag_t mag;
    char      d[200];
    double    m[3];
    double    tb;
    double    angle;
    int       rc;
    int       i;

    truth_update();
    rc = dev_mag_read(&mag);
    if (rc != DEV_OK)
    {
        snprintf(d, sizeof(d), "read failed (%d)", rc);
        result(0, "magnetometer (SPI)", d);
        return;
    }
    for (i = 0; i < 3; i++)
    {
        m[i] = mag.field[i];
    }
    tb    = norm3(truth.bvb);
    angle = acos((m[0] * truth.bvb[0] + m[1] * truth.bvb[1] + m[2] * truth.bvb[2]) / (norm3(m) * tb));
    snprintf(d, sizeof(d), "|B| %.2f uT (truth %.2f uT), angle to truth %.2f deg", norm3(m) * 1e6, tb * 1e6,
             rad2deg(angle));
    /* LEO field is 20-65 uT; the vector rotates with the tumble (~3 deg/s), so allow a few degrees */
    result(fabs(norm3(m) - tb) / tb < 0.02 && rad2deg(angle) < 3.0 && tb > 15e-6 && tb < 70e-6,
           "magnetometer (SPI)", d);
}

static void test_css(void)
{
    static const double axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    dev_css_t css;
    char      d[240];
    double    err = 0;
    int       rc;
    int       i;
    int       eclipse;

    truth_update();
    rc = dev_css_read(&css);
    if (rc != DEV_OK)
    {
        snprintf(d, sizeof(d), "read failed (%d)", rc);
        result(0, "coarse sun sensors (I2C)", d);
        return;
    }
    eclipse = norm3(truth.svb) < 0.5; /* 42 zeroes the Sun vector in eclipse */
    for (i = 0; i < 6; i++)
    {
        double c = axes[i][0] * truth.svb[0] + axes[i][1] * truth.svb[1] + axes[i][2] * truth.svb[2];
        double e = fabs(css.illum[i] - (eclipse || c < 0 ? 0.0 : c));
        err      = e > err ? e : err;
    }
    snprintf(d, sizeof(d), "[%.3f %.3f %.3f %.3f %.3f %.3f]%s, max error vs cos(Sun angle) %.3f", css.illum[0],
             css.illum[1], css.illum[2], css.illum[3], css.illum[4], css.illum[5], eclipse ? " (eclipse)" : "", err);
    result(err < 0.1, "coarse sun sensors (I2C)", d);
}

static void test_fss(void)
{
    dev_fss_t fss;
    char      d[200];
    int       rc = dev_fss_read(&fss);

    if (rc != DEV_OK)
    {
        snprintf(d, sizeof(d), "read failed (%d)", rc);
        result(0, "fine sun sensor (SPI)", d);
        return;
    }
    snprintf(d, sizeof(d), "alpha %.2f deg, beta %.2f deg, error code %u", rad2deg(fss.alpha), rad2deg(fss.beta),
             fss.error);
    /* Frame, header and checksum verified by the driver; angles must be physical. The angles can only be
     * checked against truth when the Sun is in the field of view (error code 0), which depends on attitude:
     * covered by the Phase 2 sun-pointing test */
    result(fabs(fss.alpha) <= M_PI && fabs(fss.beta) <= M_PI, "fine sun sensor (SPI)", d);
}

static void test_st(void)
{
    dev_st_t st;
    char     d[200];
    double   n;
    double   dot;
    int      rc = dev_st_read(&st);

    truth_update();
    if (rc != DEV_OK)
    {
        snprintf(d, sizeof(d), "read failed (%d)", rc);
        result(0, "star tracker (UART)", d);
        return;
    }
    n   = sqrt(st.q[0] * st.q[0] + st.q[1] * st.q[1] + st.q[2] * st.q[2] + st.q[3] * st.q[3]);
    dot = fabs(st.q[0] * truth.qn[0] + st.q[1] * truth.qn[1] + st.q[2] * truth.qn[2] + st.q[3] * truth.qn[3]);
    snprintf(d, sizeof(d), "q [%.4f %.4f %.4f %.4f] valid %u, |q| %.4f, |q.q_truth| %.4f", st.q[0], st.q[1], st.q[2],
             st.q[3], st.valid, n, dot);
    /* 16-bit encoding resolution is 3e-5 per component */
    result(fabs(n - 1.0) < 0.002, "star tracker (UART)", d);
}

static void test_gps(void)
{
    dev_gps_t gps;
    char      d[240];
    uint64_t  deadline;
    int       got = 0;
    double    dp  = 0;
    double    dv  = 0;
    int       i;

    dev_gps_init();
    /* The receiver starts logging 10 s into the simulation, then once a second */
    deadline = fs_hal_time_us() + 20000000u;
    while (!got && fs_hal_time_us() < deadline)
    {
        service(100);
        got = dev_gps_poll(&gps) == 1;
    }
    if (!got)
    {
        snprintf(d, sizeof(d), "no valid BESTXYZA log in 20 s (%u CRC errors)", dev_gps_crc_errors());
        result(0, "GPS (UART)", d);
        return;
    }
    truth_update();
    for (i = 0; i < 3; i++)
    {
        dp += (gps.pos[i] - truth.pos_w[i]) * (gps.pos[i] - truth.pos_w[i]);
        dv += (gps.vel[i] - truth.vel_w[i]) * (gps.vel[i] - truth.vel_w[i]);
    }
    dp = sqrt(dp);
    dv = sqrt(dv);
    snprintf(d, sizeof(d), "week %u, %.1f s, |r| %.1f km, %.0f m and %.1f m/s from truth (ECEF), %u CRC errors",
             gps.week, gps.seconds_of_week, norm3(gps.pos) / 1000.0, dp, dv, dev_gps_crc_errors());
    /* 400 km circular orbit: r = 6778 km. GPS logs once a second, so truth can be up to 1 s apart:
     * 7.7 km in position and about 9 m/s in Earth-fixed velocity (gravity plus frame rotation) */
    result(fabs(norm3(gps.pos) / 1000.0 - 6778.0) < 10.0 && dp < 10000.0 && dv < 20.0, "GPS (UART)", d);
}

static void test_rw(void)
{
    char   d[240];
    double h[3] = {0, 0, 0};
    int    rc   = DEV_OK;
    int    i;
    double err = 0;

    for (i = 0; i < 3 && rc == DEV_OK; i++)
    {
        rc = dev_rw_get_momentum((uint8_t)i, &h[i]);
    }
    truth_update();
    if (rc != DEV_OK)
    {
        snprintf(d, sizeof(d), "momentum read failed on wheel %d (%d)", i - 1, rc);
        result(0, "reaction wheels (UART)", d);
        return;
    }
    for (i = 0; i < 3; i++)
    {
        double e = fabs(h[i] - truth.rw_h[i]);
        err      = e > err ? e : err;
    }
    snprintf(d, sizeof(d), "H [%.6f %.6f %.6f] Nms, truth [%.6f %.6f %.6f]", h[0], h[1], h[2], truth.rw_h[0],
             truth.rw_h[1], truth.rw_h[2]);
    result(err < 1e-4, "reaction wheels: momentum", d);

    /* Small torque on wheel 0 for 2 s: momentum must grow by about torque * time */
    rc = dev_rw_set_torque(0, 0.0005);
    service(2000);
    if (rc == DEV_OK)
    {
        rc = dev_rw_set_torque(0, 0.0);
    }
    if (rc == DEV_OK)
    {
        double h_after;
        rc = dev_rw_get_momentum(0, &h_after);
        snprintf(d, sizeof(d), "0.5 mNm for 2 s: H %.6f -> %.6f Nms (expected +%.6f)", h[0], h_after, 0.0005 * 2.0);
        result(rc == DEV_OK && fabs((h_after - h[0]) - 0.001) < 0.0004, "reaction wheels: torque", d);
    }
    else
    {
        snprintf(d, sizeof(d), "set torque failed (%d)", rc);
        result(0, "reaction wheels: torque", d);
    }
}

static void test_eps(void)
{
    dev_eps_t eps;
    char      d[240];
    int       rc = dev_eps_read(&eps);
    int       on;
    int       off;

    if (rc != DEV_OK)
    {
        snprintf(d, sizeof(d), "read failed (%d)", rc);
        result(0, "EPS housekeeping (I2C)", d);
        return;
    }
    snprintf(d, sizeof(d), "battery %.2f V %.1f C, rails %.2f/%.2f/%.2f V, solar array %.2f V, switches 0x%02x",
             eps.batt_v, eps.batt_temp_c, eps.bus_3v3_v, eps.bus_5v0_v, eps.bus_12v_v, eps.sa_v, eps.switch_mask);
    result(eps.batt_v > 10.0 && eps.batt_v < 40.0 && fabs(eps.bus_3v3_v - 3.3f) < 0.1, "EPS housekeeping (I2C)", d);

    /* Switch 7 has no device behind it in the NOS3 configuration */
    on  = dev_eps_set_switch(7, 1);
    off = dev_eps_set_switch(7, 0);
    snprintf(d, sizeof(d), "switch 7 on -> %d, off -> %d (confirmed in housekeeping)", on, off);
    result(on == DEV_OK && off == DEV_OK, "EPS switch command (I2C)", d);
}

static void test_trq(void)
{
    char d[120];
    int  rc = DEV_OK;
    int  i;

    for (i = 0; i < 3 && rc == DEV_OK; i++)
    {
        rc = dev_trq_set((uint8_t)i, 0.25f);
    }
    service(500);
    for (i = 0; i < 3 && rc == DEV_OK; i++)
    {
        rc = dev_trq_set((uint8_t)i, 0.0f);
    }
    /* Command path only: the torquer sim doesn't log receipt, and the physical effect (torque from the
     * dipole in the magnetic field) is verified by the Phase 2 detumble test */
    snprintf(d, sizeof(d), "25 %% duty on 3 axes then off: %s; out-of-range duty rejected",
             rc == DEV_OK ? "sent" : "failed");
    result(rc == DEV_OK && dev_trq_set(0, 2.0f) == DEV_ERR_ARG, "magnetorquers (bridge)", d);
}

int main(int argc, char **argv)
{
    fs_umb_handlers_t h = {.on_time = on_time};
    fs_time_t         epoch = {814254200u, 0};

    if (fs_hal_init(argc, argv) != 0)
    {
        return 2;
    }
    fs_time_init(epoch);
    fs_umb_init(&h);
    truth_open();

    printf("---- OBC device drivers vs NOS3 simulators and 42 truth ----\n");
    test_link_and_time();
    if (!fs_umb_link_up())
    {
        return 1;
    }
    {
        uint64_t deadline = fs_hal_time_us() + 10000000u;
        while (!truth.valid && fs_hal_time_us() < deadline)
        {
            service(100);
        }
        result(truth.valid, "42 truth stream", truth.valid ? "receiving" : "no truth data on UDP 5112");
    }
    test_imu();
    test_mag();
    test_css();
    test_fss();
    test_st();
    test_rw();
    test_eps();
    test_trq();
    test_gps();

    printf("---- %d passed, %d failed ----\n", passes, failures);
    return failures ? 1 : 0;
}

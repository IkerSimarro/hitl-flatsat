/*
** Status display: what the FlatSat is doing, readable on the desk without a PC
**
** Eight lines of 21 characters, new text once a second. The display is written one line per pass of the 8 Hz task
** (dev_display_service), so a refresh is spread over the second. On the Pico it is the OLED; in software-in-the-
** loop the text goes to $SIL_LOG_DIR/oled.txt.
**
**   FLATSAT     SUN_POINT     mode
**   SUN   0.06 deg OK         pointing error; OK once converged
**   RATE  0.01 deg/s          body rate, from the gyro
**   WHEEL  +300/ +300 rpm     the physical wheel: measured / commanded
**   BATT 3.92V   +120mA       the EPS node's battery measurement
**   NODES ADCS ok EPS ok      CAN nodes alive
**   RF contact  -111dBm       contact with the ground, last RSSI
**   UP 01:23:45 EV 12         uptime, events so far
*/
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "fs_hal.h"
#include "obc.h"

#define RAD_TO_DEG 57.2957795f

static dev_display_text_t text;
static unsigned           tick;

/* One line of text, printf-style, cut at the display's width */
static void put(unsigned n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(unsigned n, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text[n], sizeof(text[n]), fmt, ap);
    va_end(ap);
}

static void format(void)
{
    const obc_node_t *adcs = &obc.nodes[FLATSAT_NODE_ADCS];
    const obc_node_t *eps  = &obc.nodes[FLATSAT_NODE_EPS];
    unsigned long     up   = (unsigned long)(fs_hal_time_us() / 1000000u);
    const float      *w    = obc.imu.rate;

    put(0, "FLATSAT %13s", obc_mode_name(obc.mode));
    if (obc.adcs.pointing_error >= 0.0f)
    {
        put(1, "SUN %6.2f deg %s", (double)(obc.adcs.pointing_error * RAD_TO_DEG), obc.adcs.converged ? "OK" : "");
    }
    else
    {
        put(1, "SUN   not seen");
    }
    if (obc.sensor_valid & OBC_VALID_IMU)
    {
        put(2, "RATE %5.2f deg/s", (double)(sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * RAD_TO_DEG));
    }
    else
    {
        put(2, "RATE  no gyro");
    }
    if (adcs->alive)
    {
        put(3, "WHEEL %+5d/%+5d rpm", obc.phys_wheel.meas_rpm, obc.phys_wheel.cmd_rpm);
    }
    else
    {
        put(3, "WHEEL  no ADCS node");
    }
    if (eps->alive)
    {
        put(4, "BATT %4.2fV %+6dmA", obc.eps_real.batt_mv / 1000.0, obc.eps_real.batt_ma);
    }
    else
    {
        put(4, "BATT   no EPS node");
    }
    put(5, "NODES ADCS %s EPS %s", adcs->alive ? "ok" : "--", eps->alive ? "ok" : "--");
    if (obc.comms.contact)
    {
        put(6, "RF contact %5ddBm", obc.comms.last_rssi);
    }
    else
    {
        put(6, "RF no contact");
    }
    put(7, "UP %02lu:%02lu:%02lu EV %u", up / 3600u, up / 60u % 60u, up % 60u, (unsigned)obc.event_count);
}

void obc_display_task(void)
{
    if (tick++ % 8u == 0)
    {
        format();
        dev_display_show(text); /* without a display there's nothing to do */
    }
    dev_display_service();
}

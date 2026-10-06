/*
** Sensor acquisition: reads every device, keeps the latest good values, and reports each device's
** transitions between working and failed as events (so a fault produces one event, not one per cycle)
*/
#include <stdio.h>
#include <string.h>

#include "fs_hal.h"
#include "fs_persist.h"
#include "fs_umbilical.h"
#include "obc.h"

/* Devices tracked for fault reporting, in obc_state_t.sensor_valid bit order */
static const char *const device_names[] = {"IMU", "magnetometer", "fine sun sensor", "coarse sun sensors",
                                           "star tracker", "GPS", "reaction wheels", "EPS"};

#define NUM_TRACKED    (sizeof(device_names) / sizeof(device_names[0]))
#define GPS_STALE_US   3000000u /* GPS logs once a second */
/* After the link comes up the bridge opens ten buses at ~50 ms each (NCR-004); wait well past that */
#define LINK_SETTLE_US 2000000u
/* Consecutive failed reads before a device is declared failed (NCR-005): a single late reply on a
** loaded machine marks that reading invalid but isn't a fault. About 2.5 s at each device's read rate:
** the ADCS set is read at 5 Hz, the rest at 1 Hz */
static const uint8_t fault_persistence[] = {12, 12, 3, 12, 3, 3, 12, 3};

/* A magnetometer frozen at its last value still answers with well-formed data (NCR-014). In orbit the field in
** the body frame changes by ~14 nT between 5 Hz readings (2 nT resolution), whatever the attitude, so this many
** bit-identical readings in a row mean the sensor is stuck; from then on its readings count as missed. (Not for
** the gyro: a spacecraft spinning freely about a principal axis gives constant rates.) */
#define MAG_STUCK_READS 10

#define MISS_SUMMARY_US 60000000u /* missed reads are logged as a summary once a minute */

static fs_persist_t persist[NUM_TRACKED];
static uint64_t     gps_last_fix_us;
static uint16_t     misses[NUM_TRACKED];
static uint64_t     next_summary_us;

static void reset_persistence(void)
{
    unsigned i;

    for (i = 0; i < NUM_TRACKED; i++)
    {
        fs_persist_init(&persist[i], fault_persistence[i]);
    }
}

static const char *error_text(int rc)
{
    switch (rc)
    {
        case DEV_ERR_BUS:
            return "bus error";
        case DEV_ERR_TIMEOUT:
            return "no reply";
        case DEV_ERR_FORMAT:
            return "bad reply";
        case DEV_ERR_STUCK:
            return "output frozen";
        default:
            return "error";
    }
}

static int mag_read_checked(dev_mag_t *out)
{
    static float    last[3];
    static unsigned same;
    dev_mag_t       m;
    int             rc = dev_mag_read(&m);

    if (rc != DEV_OK)
    {
        return rc;
    }
    if (memcmp(m.field, last, sizeof(last)) == 0)
    {
        if (same < MAG_STUCK_READS)
        {
            same++;
        }
    }
    else
    {
        same = 0;
        memcpy(last, m.field, sizeof(last));
    }
    if (same >= MAG_STUCK_READS)
    {
        return DEV_ERR_STUCK;
    }
    *out = m;
    return DEV_OK;
}

static void update(uint16_t bit, int rc)
{
    unsigned           idx = 0;
    fs_persist_event_t ev;

    while ((1u << idx) != bit)
    {
        idx++;
    }

    /* The data from a failed read is never used, whatever the fault state */
    if (rc == DEV_OK)
    {
        obc.sensor_valid |= bit;
    }
    else
    {
        obc.sensor_valid &= (uint16_t)~bit;
        obc.sensor_misses++;
        misses[idx]++;
    }

    ev = fs_persist_update(&persist[idx], rc == DEV_OK);
    if (ev == FS_PERSIST_TRIPPED)
    {
        obc.sensor_failed |= bit;
        obc_event(EVT_SENSOR_FAULT, FLATSAT_SEVERITY_ERROR, "%s failed: %u consecutive reads (%s)",
                  device_names[idx], fault_persistence[idx], error_text(rc));
    }
    else if (ev == FS_PERSIST_CLEARED)
    {
        obc.sensor_failed &= (uint16_t)~bit;
        obc_event(EVT_SENSOR_RECOVERED, FLATSAT_SEVERITY_INFO, "%s recovered", device_names[idx]);
    }
}

void obc_sensors_init(void)
{
    reset_persistence();
    obc.sensor_valid = 0;
    gps_last_fix_us  = 0;
    dev_open_all();
    dev_gps_init();
}

/* Without the umbilical no simulated device is reachable; the link event reports it once. Fault states are
** kept, so faults declared before the outage still get their recovery event after it. */
static int devices_reachable(void)
{
    if (!fs_umb_link_up())
    {
        obc.sensor_valid = 0;
        return 0;
    }
    return fs_umb_link_up_for_us() >= LINK_SETTLE_US;
}

void obc_sensors_acquire_adcs(void)
{
    int rc;
    int i;
    int dark = 1;

    if (!devices_reachable())
    {
        return;
    }

    update(OBC_VALID_IMU, dev_imu_read(&obc.imu));
    update(OBC_VALID_MAG, mag_read_checked(&obc.mag));

    rc = dev_css_read(&obc.css);
    update(OBC_VALID_CSS, rc);
    if (rc == DEV_OK)
    {
        for (i = 0; i < DEV_NUM_CSS; i++)
        {
            if (obc.css.illum[i] > 0.0f)
            {
                dark = 0;
            }
        }
        obc.eclipse = (uint8_t)dark;
    }

    rc = DEV_OK;
    for (i = 0; i < DEV_NUM_RW && rc == DEV_OK; i++)
    {
        rc = dev_rw_get_momentum((uint8_t)i, &obc.rw_momentum[i]);
    }
    update(OBC_VALID_RW, rc);
}

/* Local log only: isolated misses are routine on a loaded machine (NCR-005), persistent ones raise events */
static void log_miss_summary(void)
{
    char     text[160];
    size_t   n = 0;
    unsigned i;

    if (fs_hal_time_us() < next_summary_us)
    {
        return;
    }
    next_summary_us = fs_hal_time_us() + MISS_SUMMARY_US;
    for (i = 0; i < NUM_TRACKED; i++)
    {
        if (misses[i] != 0 && n < sizeof(text))
        {
            n += (size_t)snprintf(&text[n], sizeof(text) - n, "%s%s %u", n ? ", " : "", device_names[i], misses[i]);
            misses[i] = 0;
        }
    }
    if (n != 0)
    {
        fs_hal_log("sensor reads missed in the last minute: %s", text);
    }
}

void obc_sensors_acquire(void)
{
    log_miss_summary();
    if (!devices_reachable())
    {
        return;
    }

    update(OBC_VALID_FSS, dev_fss_read(&obc.fss));
    update(OBC_VALID_ST, dev_st_read(&obc.st));
    update(OBC_VALID_EPS, dev_eps_read(&obc.eps));

    /* GPS is event-driven (obc_sensors_poll_gps); here it only goes stale */
    if (gps_last_fix_us != 0)
    {
        update(OBC_VALID_GPS, fs_hal_time_us() - gps_last_fix_us < GPS_STALE_US ? DEV_OK : DEV_ERR_TIMEOUT);
    }
}

void obc_sensors_poll_gps(void)
{
    if (dev_gps_poll(&obc.gps) == 1)
    {
        gps_last_fix_us = fs_hal_time_us();
    }
}

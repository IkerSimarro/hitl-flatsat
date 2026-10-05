/*
** Sensor acquisition: reads every device, keeps the latest good values, and reports each device's
** transitions between working and failed as events (so a fault produces one event, not one per cycle)
*/
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

static fs_persist_t persist[NUM_TRACKED];
static uint64_t     gps_last_fix_us;

static void reset_persistence(void)
{
    unsigned i;

    for (i = 0; i < NUM_TRACKED; i++)
    {
        fs_persist_init(&persist[i], fault_persistence[i]);
    }
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
        fs_hal_log("%s read missed (error %d)", device_names[idx], rc); /* local log only: misses are routine */
    }

    ev = fs_persist_update(&persist[idx], rc == DEV_OK);
    if (ev == FS_PERSIST_TRIPPED)
    {
        obc.sensor_failed |= bit;
        obc_event(EVT_SENSOR_FAULT, FLATSAT_SEVERITY_ERROR, "%s failed: %u consecutive reads (last error %d)",
                  device_names[idx], fault_persistence[idx], rc);
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
    update(OBC_VALID_MAG, dev_mag_read(&obc.mag));

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

void obc_sensors_acquire(void)
{
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

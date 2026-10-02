/*
** OBC flight software: shared state and module interfaces
**
** Modules: events (obc_events.c), modes (obc_mode.c), commands (obc_cmd.c), telemetry (obc_tlm.c),
** sensor acquisition (obc_sensors.c) and the main loop (obc_main.c).
*/
#ifndef OBC_H
#define OBC_H

#include <stddef.h>
#include <stdint.h>

#include "flatsat_icd.h"
#include "fs_time.h"
#include "obc_devices.h"

/* ---- Shared state ---- */

/* Bits of obc_state_t.sensor_valid (ICD ADCS_SENSORS.VALID_MASK) */
#define OBC_VALID_IMU 0x0001u
#define OBC_VALID_MAG 0x0002u
#define OBC_VALID_FSS 0x0004u
#define OBC_VALID_CSS 0x0008u
#define OBC_VALID_ST  0x0010u
#define OBC_VALID_GPS 0x0020u
#define OBC_VALID_RW  0x0040u
#define OBC_VALID_EPS 0x0080u

typedef struct
{
    /* Mode */
    uint8_t mode;        /* flatsat_mode_t */
    uint8_t mode_reason; /* flatsat_mode_reason_t */

    /* Commanding */
    uint16_t cmd_accept_count;
    uint16_t cmd_reject_count;
    uint16_t last_cmd_mid;
    uint8_t  last_cmd_fc;

    /* Events and resets */
    uint16_t event_count;
    uint8_t  reset_cause;
    uint16_t reset_count;

    /* Latest sensor data (valid bits say which are current) */
    uint16_t  sensor_valid;
    uint16_t  sensor_misses; /* failed device reads, including ones too brief to declare a fault */
    dev_imu_t imu;
    dev_mag_t mag;
    dev_fss_t fss;
    dev_css_t css;
    dev_st_t  st;
    dev_gps_t gps;
    double    rw_momentum[DEV_NUM_RW]; /* Nms */
    dev_eps_t eps;
    uint8_t   eclipse; /* derived: every coarse sun sensor dark */

    /* Actuator commands currently applied */
    int16_t trq_duty[DEV_NUM_TRQ]; /* 0.01 % */

    /* ADCS tuning (ADCS_SET_* commands) */
    float bdot_gain;
    float sun_kp;
    float sun_kd;
} obc_state_t;

extern obc_state_t obc;

/* ---- Events (EVENT telemetry, ICD 4) ---- */

typedef enum
{
    EVT_BOOT = 1,
    EVT_CMD_NOOP,
    EVT_CMD_REJECTED,
    EVT_CMD_NOT_IMPLEMENTED,
    EVT_MODE_CHANGE,
    EVT_MODE_REFUSED,
    EVT_TIME_SET,
    EVT_TLM_PERIOD,
    EVT_SENSOR_FAULT,
    EVT_SENSOR_RECOVERED,
    EVT_ACTUATOR_FAULT,
    EVT_EPS_SWITCH,
    EVT_UMBILICAL_LINK,
    EVT_REBOOT
} obc_event_id_t;

void obc_event(uint16_t id, uint8_t severity, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---- Modes (ICD 5.1) ---- */

void obc_mode_init(void);
/* Returns 1 if the transition happened, 0 if it isn't allowed from the current mode */
int obc_mode_request(uint8_t mode, uint8_t reason);
const char *obc_mode_name(uint8_t mode);

/* ---- Commands (ICD 5) ---- */

void obc_cmd_init(void);
/* Queue a received command packet (called from the umbilical handler; no bus access allowed there) */
void obc_cmd_enqueue(const uint8_t *pkt, size_t len);
/* Validate and execute queued commands */
void obc_cmd_process(void);

/* ---- Telemetry (ICD 4) ---- */

void obc_tlm_init(void);
/* Sends every packet whose period has elapsed */
void obc_tlm_service(void);
/* Changes a packet's umbilical period (0 = off); returns 0 if the MID isn't periodic telemetry */
int obc_tlm_set_period(uint16_t mid, uint16_t period_ms);
/* Builds and sends one packet now */
void obc_tlm_send_now(uint16_t mid);
void obc_tlm_send_ping_reply(uint32_t token, fs_time_t rx_time);
/* Sends an already-built packet on every available downlink */
void obc_tlm_send_packet(const uint8_t *pkt, size_t len);

/* ---- Sensors ---- */

void obc_sensors_init(void);
/* Reads every device once and updates obc.sensor_valid; raises events on fault and recovery */
void obc_sensors_acquire(void);
/* Non-blocking: parses buffered GPS data */
void obc_sensors_poll_gps(void);

#endif /* OBC_H */

/*
** OBC flight software: shared state and module interfaces
**
** Modules: events (obc_events.c), modes (obc_mode.c), commands (obc_cmd.c), telemetry (obc_tlm.c),
** sensor acquisition (obc_sensors.c), attitude control (obc_adcs.c), CAN nodes (obc_can.c) and the main loop
** (obc_main.c).
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

/* CAN nodes the OBC tracks (ICD 6), indexed by node ID */
#define OBC_MAX_NODES 4

typedef struct
{
    uint8_t  alive;
    uint8_t  state;       /* flatsat_node_state_t from its heartbeat */
    uint8_t  reset_cause;
    uint32_t uptime;
    uint64_t last_us;
} obc_node_t;

typedef struct
{
    uint16_t rail_mv[3];
    int16_t  rail_ma[3];
    uint16_t rail_mw[3];
    uint8_t  rail_flags[3];
    uint16_t batt_mv;
    int16_t  batt_ma;
    uint8_t  charge_state;
    uint8_t  switch_mask;
    uint8_t  switch_fault_mask;
} obc_eps_real_t;

typedef struct
{
    uint8_t  cmd_mode;      /* flatsat_rw_ctrl_mode_t sent in RW_CMD */
    int16_t  cmd_rpm;
    int16_t  meas_rpm;      /* from RW_TLM */
    uint8_t  status;        /* RW_TLM status bits */
    uint8_t  tlm_fault;     /* no RW_TLM while commanded */
    uint64_t last_tlm_us;
    uint8_t  test_override; /* ADCS_PHYS_WHEEL_TEST active (TEST mode only) */
    uint8_t  test_mode;
    int16_t  test_setpoint;
} obc_phys_wheel_t;

/* Attitude control state (obc_adcs.c) */
typedef struct
{
    uint8_t adcs_mode;      /* flatsat_adcs_mode_t */
    uint8_t sun_valid;
    uint8_t converged;      /* sun pointing within ADCS_POINTED_ERROR and ADCS_POINTED_RATE */
    uint8_t auto_modes;     /* automatic mode transitions enabled (OBC_SET_AUTO_MODES) */
    float   sun[3];         /* body frame unit vector, from the coarse sun sensors */
    float   pointing_error; /* rad between +X and the Sun, -1 without a Sun vector */
    float   rw_torque[3];   /* N m commanded to the simulated wheels */
    uint8_t manual_rw[DEV_NUM_RW];       /* TEST mode: ADCS_RW_MANUAL speed loop active */
    float   manual_rw_speed[DEV_NUM_RW]; /* rad/s */
} obc_adcs_t;

/* RF link state (obc_comms.c), reported in COMMS_STATS */
typedef struct
{
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint16_t crc_errors;
    uint16_t rejected;
    int16_t  last_rssi; /* dBm */
    int8_t   last_snr;  /* 0.25 dB */
    uint8_t  contact;
    uint16_t duty_permil; /* airtime in the last hour, 0.1 % */
    uint16_t queue_depth;
    uint16_t queue_drops;
    uint16_t beacon_period_s;
    int8_t   tx_power_dbm;
} obc_comms_t;

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
    uint16_t  sensor_failed; /* devices declared failed (persistence filter tripped), same bits */
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

    /* CAN nodes, the EPS node's measurements, the physical wheel */
    obc_node_t       nodes[OBC_MAX_NODES];
    obc_eps_real_t   eps_real;
    obc_phys_wheel_t phys_wheel;

    /* RF link */
    obc_comms_t comms;

    /* Attitude control, and its tuning (ADCS_SET_* commands) */
    obc_adcs_t adcs;
    float      bdot_gain;
    float      sun_kp;
    float      sun_kd;
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
    EVT_REBOOT,
    EVT_NODE_UP,
    EVT_NODE_LOST,
    EVT_NODE_REBOOTED,
    EVT_NODE_FAULT,
    EVT_NODE_ACK,
    EVT_WHEEL_FAULT,
    EVT_ADCS,
    EVT_COMMS
} obc_event_id_t;

void obc_event(uint16_t id, uint8_t severity, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---- Modes (ICD 5.1) ---- */

void obc_mode_init(void);
/* Returns 1 if the transition happened, 0 if it isn't allowed from the current mode */
int obc_mode_request(uint8_t mode, uint8_t reason);
const char *obc_mode_name(uint8_t mode);

/* ---- Commands (ICD 5) ---- */

/* Where a command arrived from: replies go back the same way */
#define OBC_CMD_SRC_UMB 0
#define OBC_CMD_SRC_RF  1

void obc_cmd_init(void);
/* Queue a received command packet (called from receive handlers; no bus access allowed there) */
void obc_cmd_enqueue(const uint8_t *pkt, size_t len, uint8_t source);
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
/* Builds a periodic packet (any MID in the telemetry table) into pkt; returns its length, 0 if unknown */
size_t obc_tlm_build(uint16_t mid, uint8_t *pkt, size_t max);
void obc_tlm_send_ping_reply(uint32_t token, fs_time_t rx_time, uint8_t source);
/* Sends an already-built packet on every available downlink */
void obc_tlm_send_packet(const uint8_t *pkt, size_t len);

/* ---- CAN nodes (obc_can.c) ---- */

void    obc_can_init(void);
void    obc_can_poll(void);
void    obc_can_task_fast(void); /* 10 Hz: MODE on change, RW_CMD, wheel telemetry watchdog */
void    obc_can_task_slow(void); /* 1 Hz: HEARTBEAT, TIME_SYNC, MODE, node monitor */
uint8_t obc_can_alive_mask(void);
int     obc_can_switch(uint8_t sw, uint8_t on);
int     obc_can_node_command(uint8_t node, uint8_t cmd);

/* ---- RF link (obc_comms.c, ICD 7) ---- */

void obc_comms_init(void);
void obc_comms_task(void); /* 10 Hz: beacon, queued downlink, airtime budget */
/* A received RF frame (called from the umbilical receive handler: no bus access) */
void obc_comms_on_rx(const uint8_t *frame, size_t len, int16_t rssi, int8_t snr);
/* Queue a packet for the next contact; returns 0 if it can't be sent over RF */
int obc_comms_queue(const uint8_t *pkt, size_t len);
int obc_comms_set_beacon_period(uint16_t seconds); /* 0 = off, else 5..3600 */
int obc_comms_set_tx_power(int8_t dbm);

/* ---- ADCS (obc_adcs.c, docs/design/ADCS_DESIGN.md) ---- */

void obc_adcs_init(void);
/* 5 Hz: reads the ADCS sensors, runs the current mode's control law, commands the actuators */
void obc_adcs_step(void);
/* 1 Hz: fault responses and automatic mode transitions (ICD 5.1) */
void obc_adcs_auto(void);
/* Called by the mode manager on every transition: stops the actuators and resets the control laws */
void obc_adcs_mode_changed(uint8_t from, uint8_t to);

/* ---- Sensors ---- */

void obc_sensors_init(void);
/* Each read updates obc.sensor_valid and the device's fault state, with events on fault and recovery.
** 5 Hz, for the ADCS: IMU, magnetometer, coarse sun sensors, wheel momentum */
void obc_sensors_acquire_adcs(void);
/* 1 Hz: fine sun sensor, star tracker, EPS; GPS staleness */
void obc_sensors_acquire(void);
/* Non-blocking: parses buffered GPS data */
void obc_sensors_poll_gps(void);

#endif /* OBC_H */

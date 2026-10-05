/*
** OBC drivers for the NOS3-simulated spacecraft devices (ICD 8.3)
**
** Each driver speaks the device's own wire protocol, exactly as the matching NOS3 cFS component does
** (components/<name>/fsw/shared/<name>_device.c), over the umbilical client (fs_umbilical.h). Readings are
** converted to SI units here, so the rest of the flight software never sees raw device encodings.
**
** All functions return DEV_OK (0) on success or a negative DEV_ERR_* code. They block for at most the
** stated timeout and keep servicing the umbilical while they wait.
*/
#ifndef OBC_DEVICES_H
#define OBC_DEVICES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEV_OK           0
#define DEV_ERR_BUS     (-1) /* the umbilical transaction failed or the bridge reported a bus error */
#define DEV_ERR_TIMEOUT (-2) /* no reply from the device in time */
#define DEV_ERR_FORMAT  (-3) /* reply had a bad header, trailer, length or checksum */
#define DEV_ERR_ARG     (-4) /* invalid argument */

/* ---- Bus allocation, from cfg/sims/sc-1-nos3-simulator.xml and the cFS platform configs ---- */

#define DEV_IMU_CAN_BUS      0  /* can_0 */
#define DEV_IMU_CAN_ID       15
#define DEV_MAG_SPI_BUS      0  /* hwlib spi bus 0, chip select 2 -> NOS bus spi_2 */
#define DEV_MAG_SPI_CS       2
#define DEV_FSS_SPI_BUS      0  /* chip select 1 -> spi_1 */
#define DEV_FSS_SPI_CS       1
#define DEV_CSS_I2C_BUS      2  /* i2c_2 */
#define DEV_CSS_I2C_ADDR     0x40
#define DEV_EPS_I2C_BUS      1  /* i2c_1 */
#define DEV_EPS_I2C_ADDR     0x2B
#define DEV_GPS_UART         1  /* usart_1 */
#define DEV_ST_UART          10 /* usart_10 */
#define DEV_RW_UART_BASE     2  /* wheels 0..2 on usart_2..usart_4 */
#define DEV_NUM_RW           3
#define DEV_NUM_TRQ          3
#define DEV_NUM_CSS          6
#define DEV_NUM_EPS_SWITCHES 8

/* Declares every bus above to the umbilical so the bridge opens them as soon as the link is up
** (first use would otherwise add ~50 ms to the first transaction on each, see NCR-004) */
int dev_open_all(void);

/* ---- IMU (generic_imu, CAN) ---- */

typedef struct
{
    float accel[3]; /* m/s^2, body frame (42 Accel[].TrueAcc) */
    float rate[3];  /* rad/s, body frame (42 Gyro[].TrueRate) */
} dev_imu_t;

int dev_imu_read(dev_imu_t *out);

/* ---- Magnetometer (generic_mag, SPI) ---- */

typedef struct
{
    float field[3]; /* tesla, body frame */
} dev_mag_t;

int dev_mag_read(dev_mag_t *out);

/* ---- Fine sun sensor (generic_fss, SPI) ---- */

typedef struct
{
    float   alpha; /* rad, sensor frame */
    float   beta;  /* rad */
    uint8_t error; /* device error code: 0 = Sun in the field of view */
} dev_fss_t;

int dev_fss_read(dev_fss_t *out);

/* ---- Coarse sun sensors (generic_css, I2C) ---- */

typedef struct
{
    float illum[DEV_NUM_CSS]; /* normalised illumination, 0 = dark or invalid */
} dev_css_t;

int dev_css_read(dev_css_t *out);

/* ---- Star tracker (generic_star_tracker, UART) ---- */

typedef struct
{
    float   q[4];  /* attitude quaternion as reported by the device (scalar last, 42 convention) */
    uint8_t valid; /* device validity flag */
} dev_st_t;

int dev_st_read(dev_st_t *out);

/* ---- GPS (novatel_oem615, UART, NovAtel ASCII BESTXYZA log) ---- */

typedef struct
{
    uint16_t week;
    double   seconds_of_week;
    double   pos[3]; /* m, ECEF */
    double   vel[3]; /* m/s, ECEF */
} dev_gps_t;

/* Opens the port; the receiver streams BESTXYZA once a second (starting 10 s into the simulation) */
int dev_gps_init(void);
/* Parses buffered data; returns 1 and fills out when a new valid fix arrived, 0 if none, < 0 on a bad line */
int dev_gps_poll(dev_gps_t *out);
/* Lines rejected because their NovAtel CRC-32 didn't match */
uint32_t dev_gps_crc_errors(void);

/* ---- Radio (ICD 7.1; software-in-the-loop: through the umbilical to the link emulator) ---- */

int dev_radio_send(const uint8_t *frame, size_t len);
int dev_radio_set_power(int8_t dbm);

/* ---- Reaction wheels (generic_reaction_wheel, UART, ASCII) ---- */

#define DEV_RW_MAX_TORQUE 0.001 /* N m (42 SC_NOS3.txt) */

int dev_rw_get_momentum(uint8_t wheel, double *momentum_nms);
/* torque on the wheel (the body feels the opposite), up to DEV_RW_MAX_TORQUE */
int dev_rw_set_torque(uint8_t wheel, double torque_nm);

/* ---- EPS (generic_eps, I2C) ---- */

typedef struct
{
    float   batt_v;
    float   batt_temp_c;
    float   bus_3v3_v;
    float   bus_5v0_v;
    float   bus_12v_v;
    float   eps_temp_c; /* note: the NOS3 sim currently reports the 12 V bus voltage in this field */
    float   sa_v;
    float   sa_temp_c;
    float   switch_v[DEV_NUM_EPS_SWITCHES];
    float   switch_a[DEV_NUM_EPS_SWITCHES];
    uint8_t switch_mask; /* bit n set = switch n on */
} dev_eps_t;

int dev_eps_read(dev_eps_t *out);
/* Switches a simulated EPS output (ICD EPS_SIM_SWITCH); reads housekeeping back to confirm */
int dev_eps_set_switch(uint8_t sw, int on);

/* ---- Magnetorquers (generic_torquer, via the bridge) ---- */

/* duty from -1.0 to 1.0 */
int dev_trq_set(uint8_t torquer, float duty);

#ifdef __cplusplus
}
#endif

#endif /* OBC_DEVICES_H */

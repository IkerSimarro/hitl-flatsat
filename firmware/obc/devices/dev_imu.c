/*
** IMU driver (generic_imu over CAN)
**
** Each axis is requested separately with a 2-byte command {0x80, axis command}. The request and reply
** use hwlib's simulated CAN layout: a Linux struct can_frame (u32 id, u8 dlc, 3 padding bytes) followed
** by the data bytes. Each reply carries two big-endian offset-binary u32 values:
**   linear acceleration = (raw - 10 * 214748364) / 214748364    (m/s^2)
**   angular rate        = (raw - 400 * 5368709) / 5368709       (rad/s)
*/
#include <string.h>

#include "dev_internal.h"

#define IMU_HDR          0x80
#define IMU_CMD_X        0x02
#define CAN_HDR_LEN      8
#define CAN_MAX_DLEN     8
#define LIN_CONV         214748364.0
#define ANG_CONV         5368709.0

static int request_axis(uint8_t cmd, float *accel, float *rate)
{
    uint8_t  tx[CAN_HDR_LEN + 2];
    uint8_t  rx[CAN_HDR_LEN + CAN_MAX_DLEN];
    uint32_t id = DEV_IMU_CAN_ID;
    int      rc;

    memset(tx, 0, sizeof(tx));
    tx[0] = (uint8_t)(id & 0xFF); /* can_id, little-endian as in the host struct */
    tx[1] = (uint8_t)(id >> 8);
    tx[2] = (uint8_t)(id >> 16);
    tx[3] = (uint8_t)(id >> 24);
    tx[4] = 2; /* can_dlc */
    tx[CAN_HDR_LEN]     = IMU_HDR;
    tx[CAN_HDR_LEN + 1] = cmd;

    rc = fs_umb_can(DEV_IMU_CAN_BUS, id, tx, sizeof(tx), rx, sizeof(rx), DEV_TXN_TIMEOUT_MS);
    if (rc != FS_UMB_OK)
    {
        return dev_map_umb(rc);
    }
    if (rx[4] != CAN_MAX_DLEN)
    {
        return DEV_ERR_FORMAT;
    }

    *accel = (float)(((double)dev_be32(&rx[CAN_HDR_LEN]) - LIN_CONV * 10.0) / LIN_CONV);
    *rate  = (float)(((double)dev_be32(&rx[CAN_HDR_LEN + 4]) - ANG_CONV * 400.0) / ANG_CONV);
    return DEV_OK;
}

int dev_imu_read(dev_imu_t *out)
{
    int axis;
    int rc;

    for (axis = 0; axis < 3; axis++)
    {
        rc = request_axis((uint8_t)(IMU_CMD_X + axis), &out->accel[axis], &out->rate[axis]);
        if (rc != DEV_OK)
        {
            return rc;
        }
    }
    return DEV_OK;
}

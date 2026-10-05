/*
** Reaction wheel driver (generic_reaction_wheel over UART, ASCII)
**
**   "CURRENT_MOMENTUM"        -> "CURRENT_MOMENTUM=<Nms>"
**   "SET_TORQUE=<%.6e Nm>"    -> "SET_TORQUE=<Nm>"
** NOS3's cFS driver formats the torque as %10.4f: 0.1 mN m steps on a 1 mN m wheel, which rounds fine-pointing
** torques to zero. The simulator parses any floating-point format.
** Replies have no terminator, so a reply is complete when the line goes quiet. NOS3's cFS driver reads
** immediately after writing, which only works on a zero-latency simulated bus.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_internal.h"

#define RW_TIMEOUT_MS 100
#define RW_QUIET_MS   5
#define RW_REPLY_MAX  64

static int request(uint8_t wheel, const char *req, char *reply)
{
    int n;

    if (wheel >= DEV_NUM_RW)
    {
        return DEV_ERR_ARG;
    }
    n = dev_uart_request((uint8_t)(DEV_RW_UART_BASE + wheel), (const uint8_t *)req, strlen(req), (uint8_t *)reply,
                         RW_REPLY_MAX - 1, 0, RW_TIMEOUT_MS, RW_QUIET_MS);
    if (n < 0)
    {
        return n;
    }
    reply[n] = '\0';
    return DEV_OK;
}

int dev_rw_get_momentum(uint8_t wheel, double *momentum_nms)
{
    static const char prefix[] = "CURRENT_MOMENTUM=";
    char              reply[RW_REPLY_MAX];
    char             *end;
    int               rc = request(wheel, "CURRENT_MOMENTUM", reply);

    if (rc != DEV_OK)
    {
        return rc;
    }
    if (strncmp(reply, prefix, sizeof(prefix) - 1) != 0)
    {
        return DEV_ERR_FORMAT;
    }
    *momentum_nms = strtod(reply + sizeof(prefix) - 1, &end);
    return end == reply + sizeof(prefix) - 1 ? DEV_ERR_FORMAT : DEV_OK;
}

int dev_rw_set_torque(uint8_t wheel, double torque_nm)
{
    char req[32];
    char reply[RW_REPLY_MAX];
    int  rc;

    if (!(torque_nm >= -DEV_RW_MAX_TORQUE && torque_nm <= DEV_RW_MAX_TORQUE)) /* also rejects NaN */
    {
        return DEV_ERR_ARG;
    }
    snprintf(req, sizeof(req), "SET_TORQUE=%.6e", torque_nm);
    rc = request(wheel, req, reply);
    if (rc != DEV_OK)
    {
        return rc;
    }
    return strncmp(reply, "SET_TORQUE=", 11) == 0 ? DEV_OK : DEV_ERR_FORMAT;
}

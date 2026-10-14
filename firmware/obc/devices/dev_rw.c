/*
** Reaction wheel driver (generic_reaction_wheel over UART, ASCII)
**
**   "CURRENT_MOMENTUM"        -> "CURRENT_MOMENTUM=<Nms>"
**   "SET_TORQUE=<%.6e Nm>"    -> "SET_TORQUE=<Nm>"
** NOS3's cFS driver formats the torque as %10.4f: 0.1 mN m steps on a 1 mN m wheel, which rounds fine-pointing
** torques to zero. The simulator parses any floating-point format.
** Torques below 1e-12 N m (far below the wheel's resolution) are sent as 0. That also turns a negative zero
** (a wheel torque is the negated body torque) into "0": the Pico SDK's printf formatted -0.0 as "nane-308",
** which the simulator read as NaN and passed to 42 (NCR-017).
** Replies have no terminator, so a reply is complete when the line goes quiet. NOS3's cFS driver reads
** immediately after writing, which only works on a zero-latency simulated bus.
*/
#include <math.h>
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

/* The three wheels at once (each on its own UART): one round trip instead of three (the 5 Hz ADCS budget) */
static void request_all(const char *const req[DEV_NUM_RW], char reply[DEV_NUM_RW][RW_REPLY_MAX], int rc[DEV_NUM_RW])
{
    uint8_t  bus[DEV_NUM_RW];
    uint8_t *rx[DEV_NUM_RW];
    unsigned w;

    for (w = 0; w < DEV_NUM_RW; w++)
    {
        bus[w] = (uint8_t)(DEV_RW_UART_BASE + w);
        rx[w]  = (uint8_t *)reply[w];
    }
    dev_uart_request_all(DEV_NUM_RW, bus, req, rx, RW_REPLY_MAX - 1, RW_TIMEOUT_MS, RW_QUIET_MS, rc);
    for (w = 0; w < DEV_NUM_RW; w++)
    {
        reply[w][rc[w] >= 0 ? rc[w] : 0] = '\0';
        rc[w]                            = rc[w] >= 0 ? DEV_OK : rc[w];
    }
}

static int parse_momentum(const char *reply, double *momentum_nms)
{
    static const char prefix[] = "CURRENT_MOMENTUM=";
    char             *end;

    if (strncmp(reply, prefix, sizeof(prefix) - 1) != 0)
    {
        return DEV_ERR_FORMAT;
    }
    *momentum_nms = strtod(reply + sizeof(prefix) - 1, &end);
    return end == reply + sizeof(prefix) - 1 ? DEV_ERR_FORMAT : DEV_OK;
}

int dev_rw_get_momentum(uint8_t wheel, double *momentum_nms)
{
    char reply[RW_REPLY_MAX];
    int  rc = request(wheel, "CURRENT_MOMENTUM", reply);

    return rc != DEV_OK ? rc : parse_momentum(reply, momentum_nms);
}

int dev_rw_get_momentum_all(double momentum_nms[DEV_NUM_RW])
{
    static const char *const req[DEV_NUM_RW] = {"CURRENT_MOMENTUM", "CURRENT_MOMENTUM", "CURRENT_MOMENTUM"};
    char                     reply[DEV_NUM_RW][RW_REPLY_MAX];
    int                      rc[DEV_NUM_RW];
    int                      result = DEV_OK;
    unsigned                 w;

    request_all(req, reply, rc);
    for (w = 0; w < DEV_NUM_RW; w++)
    {
        int r = rc[w] != DEV_OK ? rc[w] : parse_momentum(reply[w], &momentum_nms[w]);
        result = result == DEV_OK ? r : result;
    }
    return result;
}

/* The request for a torque, or DEV_ERR_ARG if it is out of range */
static int torque_request(double torque_nm, char req[32])
{
    if (!(torque_nm >= -DEV_RW_MAX_TORQUE && torque_nm <= DEV_RW_MAX_TORQUE)) /* also rejects NaN */
    {
        return DEV_ERR_ARG;
    }
    if (fabs(torque_nm) < DEV_RW_MIN_TORQUE)
    {
        torque_nm = 0.0;
    }
    snprintf(req, 32, "SET_TORQUE=%.6e", torque_nm);
    return DEV_OK;
}

int dev_rw_set_torque(uint8_t wheel, double torque_nm)
{
    char req[32];
    char reply[RW_REPLY_MAX];
    int  rc = torque_request(torque_nm, req);

    if (rc == DEV_OK)
    {
        rc = request(wheel, req, reply);
    }
    if (rc != DEV_OK)
    {
        return rc;
    }
    return strncmp(reply, "SET_TORQUE=", 11) == 0 ? DEV_OK : DEV_ERR_FORMAT;
}

int dev_rw_set_torque_all(const double torque_nm[DEV_NUM_RW])
{
    char        req[DEV_NUM_RW][32];
    const char *reqs[DEV_NUM_RW];
    char        reply[DEV_NUM_RW][RW_REPLY_MAX];
    int         rc[DEV_NUM_RW];
    int         result = DEV_OK;
    unsigned    w;

    for (w = 0; w < DEV_NUM_RW; w++)
    {
        if (torque_request(torque_nm[w], req[w]) != DEV_OK)
        {
            return DEV_ERR_ARG; /* none is sent: the wheels are commanded together or not at all */
        }
        reqs[w] = req[w];
    }
    request_all(reqs, reply, rc);
    for (w = 0; w < DEV_NUM_RW; w++)
    {
        int r = rc[w] != DEV_OK ? rc[w] : (strncmp(reply[w], "SET_TORQUE=", 11) == 0 ? DEV_OK : DEV_ERR_FORMAT);
        result = result == DEV_OK ? r : result;
    }
    return result;
}

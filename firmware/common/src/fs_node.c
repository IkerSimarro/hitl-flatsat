/*
** Services every FlatSat CAN node runs (see fs_node.h)
*/
#include "fs_node.h"

#include <string.h>

#include "flatsat_icd.h"
#include "fs_can.h"
#include "fs_hal.h"
#include "fs_time.h"

static fs_node_callbacks_t cb;
static uint64_t            boot_us;
static uint64_t            next_heartbeat_us;
static uint64_t            obc_last_us;
static int                 obc_alive;
static uint8_t             obc_mode;

static void on_heartbeat(uint8_t src, const void *payload, uint8_t dlc)
{
    (void)payload;
    (void)dlc;
    if (src == FLATSAT_NODE_OBC)
    {
        obc_last_us = fs_hal_time_us();
        obc_alive   = 1;
    }
}

static void on_time_sync(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_time_sync_t *t = payload;
    fs_time_t                      ref;

    (void)dlc;
    if (src == FLATSAT_NODE_OBC)
    {
        ref.seconds    = t->seconds;
        ref.subseconds = t->subseconds;
        fs_time_sync(ref, 1);
    }
}

static void on_mode(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_mode_t *m = payload;

    (void)dlc;
    if (src == FLATSAT_NODE_OBC && m->mode != obc_mode)
    {
        obc_mode = m->mode;
        if (cb.mode_changed)
        {
            cb.mode_changed(obc_mode);
        }
    }
}

static void on_node_cmd(uint8_t src, const void *payload, uint8_t dlc)
{
    const flatsat_can_node_cmd_t *c = payload;
    flatsat_can_node_ack_t        ack;

    (void)dlc;
    if (src != FLATSAT_NODE_OBC || c->target != fs_can_own_node())
    {
        return;
    }

    memset(&ack, 0, sizeof(ack));
    ack.cmd    = c->cmd;
    ack.result = 0;
    switch (c->cmd)
    {
        case FLATSAT_NODE_CMD_PING:
            fs_can_send(FLATSAT_CAN_NODE_ACK_TYPE, &ack);
            break;
        case FLATSAT_NODE_CMD_ENTER_SAFE:
            fs_can_send(FLATSAT_CAN_NODE_ACK_TYPE, &ack);
            if (cb.enter_safe)
            {
                cb.enter_safe("commanded by the OBC");
            }
            break;
        case FLATSAT_NODE_CMD_RESET:
            fs_can_send(FLATSAT_CAN_NODE_ACK_TYPE, &ack);
            fs_hal_reboot();
            break;
        default:
            ack.result = 1;
            fs_can_send(FLATSAT_CAN_NODE_ACK_TYPE, &ack);
            break;
    }
}

void fs_node_init(uint8_t own_node, const fs_node_callbacks_t *callbacks)
{
    memset(&cb, 0, sizeof(cb));
    if (callbacks)
    {
        cb = *callbacks;
    }
    fs_can_init(own_node);
    fs_can_subscribe(FLATSAT_CAN_HEARTBEAT_TYPE, on_heartbeat);
    fs_can_subscribe(FLATSAT_CAN_TIME_SYNC_TYPE, on_time_sync);
    fs_can_subscribe(FLATSAT_CAN_MODE_TYPE, on_mode);
    fs_can_subscribe(FLATSAT_CAN_NODE_CMD_TYPE, on_node_cmd);

    boot_us           = fs_hal_time_us();
    next_heartbeat_us = boot_us;
    obc_last_us       = 0;
    obc_alive         = 0;
    obc_mode          = FLATSAT_MODE_SAFE;
}

void fs_node_service(void)
{
    uint64_t now = fs_hal_time_us();

    if (now >= next_heartbeat_us)
    {
        flatsat_can_heartbeat_t hb;

        memset(&hb, 0, sizeof(hb));
        hb.uptime      = fs_node_uptime_s();
        hb.state       = cb.state ? cb.state() : FLATSAT_NODE_STATE_NOMINAL;
        hb.reset_cause = fs_hal_reset_cause();
        fs_hal_can_error_counters(&hb.tec, &hb.rec);
        fs_can_send(FLATSAT_CAN_HEARTBEAT_TYPE, &hb);
        next_heartbeat_us += FS_NODE_HEARTBEAT_PERIOD_US;
        if (next_heartbeat_us <= now)
        {
            next_heartbeat_us = now + FS_NODE_HEARTBEAT_PERIOD_US;
        }
    }

    /* OBC lost: safe the node once; it stays safe until the OBC says otherwise (MODE or commands) */
    if (obc_alive && now - obc_last_us > FS_NODE_OBC_TIMEOUT_US)
    {
        obc_alive = 0;
        if (cb.enter_safe)
        {
            cb.enter_safe("OBC heartbeat lost");
        }
    }
}

int fs_node_obc_alive(void)
{
    return obc_alive;
}

uint8_t fs_node_obc_mode(void)
{
    return obc_mode;
}

uint32_t fs_node_uptime_s(void)
{
    return (uint32_t)((fs_hal_time_us() - boot_us) / 1000000u);
}

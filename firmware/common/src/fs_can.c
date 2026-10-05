/*
** FlatSat CAN messaging (see fs_can.h)
*/
#include "fs_can.h"

#include <string.h>

#include "fs_hal.h"

#define NUM_TYPES 128

/* DLC per message type from the generated ICD table; 0xFF = not a FlatSat message */
static uint8_t          dlc_of[NUM_TYPES];
static fs_can_handler_t handlers[NUM_TYPES];
static uint8_t          own;
static fs_can_stats_t   stats;

void fs_can_init(uint8_t own_node)
{
    memset(dlc_of, 0xFF, sizeof(dlc_of));
#define SET_DLC(name, type, dlc, period) dlc_of[type] = dlc;
    FLATSAT_CAN_LIST(SET_DLC)
#undef SET_DLC
    memset(handlers, 0, sizeof(handlers));
    memset(&stats, 0, sizeof(stats));
    own = own_node;
}

uint8_t fs_can_own_node(void)
{
    return own;
}

int fs_can_send(uint8_t type, const void *payload)
{
    fs_can_frame_t f;

    if (type >= NUM_TYPES || dlc_of[type] == 0xFF)
    {
        stats.tx_errors++;
        return -1;
    }
    f.id  = FLATSAT_CAN_ID(type, own);
    f.dlc = dlc_of[type];
    memset(f.data, 0, sizeof(f.data));
    memcpy(f.data, payload, f.dlc);
    if (fs_hal_can_send(&f) != 0)
    {
        stats.tx_errors++;
        return -1;
    }
    stats.tx++;
    return 0;
}

void fs_can_subscribe(uint8_t type, fs_can_handler_t handler)
{
    if (type < NUM_TYPES)
    {
        handlers[type] = handler;
    }
}

void fs_can_poll(void)
{
    fs_can_frame_t f;
    int            rc;

    while ((rc = fs_hal_can_recv(&f)) == 1)
    {
        uint8_t type = FLATSAT_CAN_TYPE(f.id);
        uint8_t src  = FLATSAT_CAN_NODE(f.id);

        if (src == own)
        {
            continue; /* our own frame looped back by the bus */
        }
        if (type >= NUM_TYPES || dlc_of[type] == 0xFF || f.dlc != dlc_of[type])
        {
            stats.rx_errors++;
            continue;
        }
        stats.rx++;
        if (handlers[type] != NULL)
        {
            /* Aligned copy, so handlers can read it through the ICD payload structs */
            union
            {
                uint8_t  bytes[8];
                uint64_t align;
            } payload;
            memcpy(payload.bytes, f.data, sizeof(payload.bytes));
            handlers[type](src, payload.bytes, f.dlc);
        }
    }
}

const fs_can_stats_t *fs_can_stats(void)
{
    return &stats;
}

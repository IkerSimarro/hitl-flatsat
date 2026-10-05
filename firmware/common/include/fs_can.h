/*
** FlatSat CAN messaging (ICD 6): typed messages over fs_hal CAN frames
**
** CAN ID = message type << 4 | source node. Received frames are checked against the generated message
** table (known type, exact DLC) and dispatched to the handler subscribed for their type; frames this
** node sent itself and malformed frames are dropped and counted.
*/
#ifndef FS_CAN_H
#define FS_CAN_H

#include <stdint.h>

#include "flatsat_icd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fs_can_handler_t)(uint8_t src_node, const void *payload, uint8_t dlc);

typedef struct
{
    uint32_t tx;
    uint32_t rx;
    uint32_t tx_errors;
    uint32_t rx_errors; /* unknown type or wrong length */
} fs_can_stats_t;

void fs_can_init(uint8_t own_node);
uint8_t fs_can_own_node(void);

/* Sends message `type` with the payload struct from flatsat_icd.h; the DLC comes from the ICD table */
int fs_can_send(uint8_t type, const void *payload);

/* One handler per message type; a later call replaces the earlier one */
void fs_can_subscribe(uint8_t type, fs_can_handler_t handler);

/* Reads and dispatches every waiting frame */
void fs_can_poll(void);

const fs_can_stats_t *fs_can_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_CAN_H */

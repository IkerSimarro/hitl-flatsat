/*
** Services every FlatSat CAN node runs (ICD 6.3, 6.5)
**
** - HEARTBEAT every second with the node's state, uptime, CAN error counters and reset cause
** - watches the OBC's heartbeat: after 3 s of silence the node goes to its local safe state, once
** - TIME_SYNC from the OBC steps the node's mission clock; MODE is passed to the node
** - NODE_CMD addressed to this node: PING (acknowledged), RESET (acknowledged, then reboot),
**   ENTER_SAFE (acknowledged, then safe state)
**
** Uses fs_can (subscribes to HEARTBEAT, TIME_SYNC, MODE and NODE_CMD); call fs_can_poll() and
** fs_node_service() from the main loop.
*/
#ifndef FS_NODE_H
#define FS_NODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FS_NODE_HEARTBEAT_PERIOD_US 1000000u
#define FS_NODE_OBC_TIMEOUT_US      3000000u

typedef struct
{
    /* The node must stop its actuators. Called once per loss of the OBC heartbeat, and on ENTER_SAFE. */
    void (*enter_safe)(const char *reason);
    /* OBC system mode changed (MODE message) */
    void (*mode_changed)(uint8_t mode);
    /* Node state for the heartbeat (flatsat_node_state_t) */
    uint8_t (*state)(void);
} fs_node_callbacks_t;

void fs_node_init(uint8_t own_node, const fs_node_callbacks_t *callbacks);

/* Sends the heartbeat when due and checks the OBC heartbeat */
void fs_node_service(void);

int     fs_node_obc_alive(void);
uint8_t fs_node_obc_mode(void);
uint32_t fs_node_uptime_s(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_NODE_H */

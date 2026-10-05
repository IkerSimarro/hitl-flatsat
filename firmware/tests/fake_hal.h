/*
** Fake HAL for unit tests: simulated time and a scripted HIL bridge on the umbilical
*/
#ifndef FAKE_HAL_H
#define FAKE_HAL_H

#include <stddef.h>
#include <stdint.h>

/* Simulated monotonic clock; fs_hal_sleep_us() advances it */
extern uint64_t fake_now_us;

/* When set, the fake bridge ignores requests (to test timeouts) */
extern int fake_bridge_silent;

/* Status the fake bridge returns for transactions (hil_status_t) */
extern uint8_t fake_bridge_status;

/* Last frame the firmware sent, as decoded by the fake bridge */
extern uint8_t  fake_last_type;
extern uint8_t  fake_last_bus;
extern uint32_t fake_last_addr;
extern uint8_t  fake_last_payload[256];
extern size_t   fake_last_len;
extern unsigned fake_frames_from_fw;

/* Queue an unsolicited frame from the bridge to the firmware */
void fake_bridge_inject(uint8_t type, uint8_t bus, const uint8_t *payload, size_t len);

/* Fake CAN bus: frames the firmware sends are recorded; frames injected here are what it receives */
#include "fs_hal.h"
#define FAKE_CAN_MAX 64
extern fs_can_frame_t fake_can_sent[FAKE_CAN_MAX];
extern unsigned       fake_can_sent_count;
void fake_can_inject(uint16_t id, const void *data, uint8_t dlc);
extern int fake_reboots;

void fake_hal_reset(void);

#endif /* FAKE_HAL_H */

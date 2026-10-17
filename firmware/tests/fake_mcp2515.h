/*
** Register-level model of the MCP2515 CAN controller, behind the driver's SPI callback (tests/test_drivers.c):
** its registers, SPI instructions and the side effects the driver relies on (mode changes, transmit requests,
** receive buffers and their flags, receive overflow).
*/
#ifndef FAKE_MCP2515_H
#define FAKE_MCP2515_H

#include <stddef.h>
#include <stdint.h>

#include "fs_hal.h"

extern uint8_t fake_mcp2515_regs[128];
extern int     fake_mcp2515_present;  /* 0: nothing on the bus, every read returns 0xFF */
extern int     fake_mcp2515_tx_hold;  /* 1: transmit requests stay pending */
extern unsigned       fake_mcp2515_sent_count;
extern fs_can_frame_t fake_mcp2515_sent[16]; /* frames put on the bus, in order */

void fake_mcp2515_reset(void);
/* The SPI callback for mcp2515_t */
int fake_mcp2515_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
/* Sends every pending transmit request */
void fake_mcp2515_complete_tx(void);
/* A frame from the bus, into RXB0, or RXB1 if RXB0 is full and rollover is on; else an overflow */
void fake_mcp2515_receive(uint32_t id, int extended, int remote, uint8_t dlc, const uint8_t *data);

#endif /* FAKE_MCP2515_H */

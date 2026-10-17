/*
** MCP2515 CAN controller (Waveshare Pico-CAN-B: 16 MHz crystal, SIT65HVD230 transceiver, ICD 6.1)
**
** Portable: the chip is reached through an SPI transfer callback, so the same code runs on the Pico (SPI0,
** GP4-GP7) and in the unit tests against a register-level model of the chip (tests/fake_mcp2515.c).
**
** 500 kbit/s from 16 MHz: BRP 0 (125 ns time quanta), 16 quanta per bit: sync 1, propagation 5, phase 1 8,
** phase 2 2, so the bit is sampled at 87.5 % (CiA 301's recommendation); SJW 1. Every node uses this driver,
** so every node samples at the same point.
**
** The receive path is polled: the OBC and the nodes service CAN in their main loops, and the controller's two
** receive buffers hold frames until then (at 500 kbit/s the FlatSat's traffic is below 1 % of the bus, ICD 6.6).
*/
#ifndef MCP2515_H
#define MCP2515_H

#include <stddef.h>
#include <stdint.h>

#include "fs_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* One SPI transaction: chip select held low for all len bytes; rx may be NULL. Returns 0 on success. */
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void *ctx;

    /* Maintained by the driver */
    uint32_t rx_overflows; /* frames the controller dropped: both receive buffers were full */
    uint32_t bus_off;      /* times the controller went bus-off and was recovered */
    uint8_t  was_bus_off;
} mcp2515_t;

#define MCP2515_OK        0
#define MCP2515_ERR_SPI   (-1) /* the transfer failed */
#define MCP2515_ERR_CHIP  (-2) /* no MCP2515 answering as one should (not fitted, or not powered) */
#define MCP2515_ERR_MODE  (-3) /* it didn't enter the requested mode */
#define MCP2515_ERR_BUSY  (-4) /* all three transmit buffers are still sending */
#define MCP2515_ERR_FRAME (-5) /* invalid frame (identifier or length) */

/* Resets the controller, checks it answers, sets 500 kbit/s, accepts every frame, enters normal mode */
int mcp2515_init(mcp2515_t *d);
/* Queues a classic 11-bit data frame in a free transmit buffer */
int mcp2515_send(mcp2515_t *d, const fs_can_frame_t *frame);
/* 1 and the frame if one was waiting, 0 if none, < 0 on error. Extended and remote frames are dropped. */
int mcp2515_recv(mcp2515_t *d, fs_can_frame_t *frame);
/* Transmit and receive error counters (TEC, REC) */
int mcp2515_error_counters(mcp2515_t *d, uint8_t *tec, uint8_t *rec);
/* Clears receive overflows (counted), and restarts the controller after bus-off (counted). Call periodically. */
int mcp2515_service(mcp2515_t *d);

/* Registers and instructions (datasheet DS20001801), for the driver and the chip model */
#define MCP2515_CMD_RESET     0xC0
#define MCP2515_CMD_READ      0x03
#define MCP2515_CMD_WRITE     0x02
#define MCP2515_CMD_BITMOD    0x05
#define MCP2515_CMD_STATUS    0xA0
#define MCP2515_CMD_LOAD_TX   0x40 /* | 0, 2, 4: TXB0, TXB1, TXB2 from SIDH */
#define MCP2515_CMD_RTS       0x80 /* | 1, 2, 4: TXB0, TXB1, TXB2 */
#define MCP2515_CMD_READ_RX   0x90 /* | 0, 4: RXB0, RXB1 from SIDH; reading clears the buffer's RXnIF */

#define MCP2515_REG_CANSTAT   0x0E
#define MCP2515_REG_CANCTRL   0x0F
#define MCP2515_REG_TEC       0x1C
#define MCP2515_REG_REC       0x1D
#define MCP2515_REG_CNF3      0x28
#define MCP2515_REG_CNF2      0x29
#define MCP2515_REG_CNF1      0x2A
#define MCP2515_REG_CANINTE   0x2B
#define MCP2515_REG_CANINTF   0x2C
#define MCP2515_REG_EFLG      0x2D
#define MCP2515_REG_TXB0CTRL  0x30
#define MCP2515_REG_RXB0CTRL  0x60
#define MCP2515_REG_RXB1CTRL  0x70

#define MCP2515_MODE_NORMAL   0x00
#define MCP2515_MODE_CONFIG   0x80
#define MCP2515_MODE_MASK     0xE0

#define MCP2515_CNF1_500K     0x00 /* SJW 1, BRP 0 */
#define MCP2515_CNF2_500K     0xBC /* BTLMODE, single sample, PHSEG1 8, PRSEG 5 */
#define MCP2515_CNF3_500K     0x01 /* PHSEG2 2 */

#ifdef __cplusplus
}
#endif

#endif /* MCP2515_H */

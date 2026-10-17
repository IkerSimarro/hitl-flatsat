/*
** MCP2515 CAN controller (see mcp2515.h)
*/
#include "mcp2515.h"

#include <string.h>

#define STATUS_RX0IF  0x01
#define STATUS_RX1IF  0x02
#define STATUS_TXREQ0 0x04
#define STATUS_TXREQ1 0x10
#define STATUS_TXREQ2 0x40

#define SIDL_IDE      0x08 /* extended identifier */
#define SIDL_SRR      0x10 /* standard remote frame (received) */
#define EFLG_RXOVR    0xC0 /* RX1OVR | RX0OVR */
#define EFLG_TXBO     0x20 /* bus-off */
#define RXBCTRL_ANY   0x60 /* RXM = 11: filters and masks off, every frame accepted */
#define RXB0CTRL_BUKT 0x04 /* a frame for a full RXB0 rolls over into RXB1 */
#define MODE_POLLS    100  /* 100 x 100 us for a mode change */

static int xfer(mcp2515_t *d, const uint8_t *tx, uint8_t *rx, size_t len)
{
    return d->xfer(d->ctx, tx, rx, len) == 0 ? MCP2515_OK : MCP2515_ERR_SPI;
}

static int read_regs(mcp2515_t *d, uint8_t addr, uint8_t *out, size_t n)
{
    uint8_t tx[2 + 14] = {MCP2515_CMD_READ, addr};
    uint8_t rx[2 + 14];
    int     rc;

    if (n > 14)
    {
        return MCP2515_ERR_SPI;
    }
    rc = xfer(d, tx, rx, 2 + n);
    if (rc == MCP2515_OK)
    {
        memcpy(out, &rx[2], n);
    }
    return rc;
}

static int write_regs(mcp2515_t *d, uint8_t addr, const uint8_t *data, size_t n)
{
    uint8_t tx[2 + 14] = {MCP2515_CMD_WRITE, addr};

    if (n > 14)
    {
        return MCP2515_ERR_SPI;
    }
    memcpy(&tx[2], data, n);
    return xfer(d, tx, NULL, 2 + n);
}

static int bit_modify(mcp2515_t *d, uint8_t addr, uint8_t mask, uint8_t data)
{
    const uint8_t tx[4] = {MCP2515_CMD_BITMOD, addr, mask, data};
    return xfer(d, tx, NULL, sizeof(tx));
}

static int read_status(mcp2515_t *d, uint8_t *status)
{
    const uint8_t tx[2] = {MCP2515_CMD_STATUS, 0};
    uint8_t       rx[2];
    int           rc = xfer(d, tx, rx, sizeof(tx));

    *status = rx[1];
    return rc;
}

static int set_mode(mcp2515_t *d, uint8_t mode)
{
    uint8_t canstat = 0;
    int     i;
    int     rc = bit_modify(d, MCP2515_REG_CANCTRL, MCP2515_MODE_MASK, mode);

    for (i = 0; rc == MCP2515_OK && i < MODE_POLLS; i++)
    {
        rc = read_regs(d, MCP2515_REG_CANSTAT, &canstat, 1);
        if (rc == MCP2515_OK && (canstat & MCP2515_MODE_MASK) == mode)
        {
            return MCP2515_OK;
        }
        fs_hal_sleep_us(100);
    }
    return rc != MCP2515_OK ? rc : MCP2515_ERR_MODE;
}

int mcp2515_init(mcp2515_t *d)
{
    static const uint8_t cnf[3] = {MCP2515_CNF3_500K, MCP2515_CNF2_500K, MCP2515_CNF1_500K}; /* CNF3, CNF2, CNF1 */
    const uint8_t        reset  = MCP2515_CMD_RESET;
    const uint8_t        zero[2] = {0, 0};
    const uint8_t        rxb0    = RXBCTRL_ANY | RXB0CTRL_BUKT;
    const uint8_t        rxb1    = RXBCTRL_ANY;
    uint8_t              canstat = 0;
    uint8_t              check[3];
    int                  rc;

    d->rx_overflows = 0;
    d->bus_off      = 0;
    d->was_bus_off  = 0;
    rc = xfer(d, &reset, NULL, 1);
    if (rc != MCP2515_OK)
    {
        return rc;
    }
    fs_hal_sleep_us(100); /* 128 oscillator cycles, and the oscillator start-up */

    /* After a reset the controller is in configuration mode. A missing chip reads 0x00 or 0xFF; to tell a chip
    ** that answers from a bus that floats, the bit timing written next must read back as well. */
    rc = read_regs(d, MCP2515_REG_CANSTAT, &canstat, 1);
    if (rc != MCP2515_OK)
    {
        return rc;
    }
    if ((canstat & MCP2515_MODE_MASK) != MCP2515_MODE_CONFIG)
    {
        return MCP2515_ERR_CHIP;
    }
    rc = write_regs(d, MCP2515_REG_CNF3, cnf, sizeof(cnf));
    if (rc == MCP2515_OK)
    {
        rc = read_regs(d, MCP2515_REG_CNF3, check, sizeof(check));
    }
    if (rc != MCP2515_OK)
    {
        return rc;
    }
    if (memcmp(check, cnf, sizeof(cnf)) != 0)
    {
        return MCP2515_ERR_CHIP;
    }

    /* Polled: no interrupt outputs. Every frame accepted, RXB0 rolling over into RXB1. Flags clear. */
    rc = write_regs(d, MCP2515_REG_CANINTE, zero, 2); /* CANINTE, CANINTF */
    if (rc == MCP2515_OK)
    {
        rc = write_regs(d, MCP2515_REG_EFLG, zero, 1);
    }
    if (rc == MCP2515_OK)
    {
        rc = write_regs(d, MCP2515_REG_RXB0CTRL, &rxb0, 1);
    }
    if (rc == MCP2515_OK)
    {
        rc = write_regs(d, MCP2515_REG_RXB1CTRL, &rxb1, 1);
    }
    return rc != MCP2515_OK ? rc : set_mode(d, MCP2515_MODE_NORMAL);
}

int mcp2515_send(mcp2515_t *d, const fs_can_frame_t *frame)
{
    static const uint8_t txreq[3] = {STATUS_TXREQ0, STATUS_TXREQ1, STATUS_TXREQ2};
    uint8_t              tx[1 + 5 + 8];
    uint8_t              status;
    uint8_t              rts;
    unsigned             n;
    int                  rc;

    if (frame->id > 0x7FF || frame->dlc > 8)
    {
        return MCP2515_ERR_FRAME;
    }
    rc = read_status(d, &status);
    if (rc != MCP2515_OK)
    {
        return rc;
    }
    for (n = 0; n < 3 && (status & txreq[n]); n++)
    {
    }
    if (n == 3)
    {
        return MCP2515_ERR_BUSY;
    }

    tx[0] = (uint8_t)(MCP2515_CMD_LOAD_TX | (n * 2)); /* from TXBnSIDH */
    tx[1] = (uint8_t)(frame->id >> 3);                /* SIDH */
    tx[2] = (uint8_t)((frame->id & 0x07) << 5);       /* SIDL: standard identifier */
    tx[3] = 0;                                        /* EID8 */
    tx[4] = 0;                                        /* EID0 */
    tx[5] = frame->dlc;                               /* DLC, data frame */
    memcpy(&tx[6], frame->data, frame->dlc);
    rc = xfer(d, tx, NULL, 6u + frame->dlc);
    if (rc != MCP2515_OK)
    {
        return rc;
    }
    rts = (uint8_t)(MCP2515_CMD_RTS | (1u << n));
    return xfer(d, &rts, NULL, 1);
}

int mcp2515_recv(mcp2515_t *d, fs_can_frame_t *frame)
{
    uint8_t tx[1 + 13] = {0};
    uint8_t rx[1 + 13];
    uint8_t status;
    int     rc;

    for (;;)
    {
        rc = read_status(d, &status);
        if (rc != MCP2515_OK)
        {
            return rc;
        }
        if (!(status & (STATUS_RX0IF | STATUS_RX1IF)))
        {
            return 0;
        }
        /* SIDH, SIDL, EID8, EID0, DLC, D0-D7; ending the transfer clears the buffer's RXnIF */
        tx[0] = (uint8_t)(MCP2515_CMD_READ_RX | ((status & STATUS_RX0IF) ? 0 : 4));
        rc    = xfer(d, tx, rx, sizeof(tx));
        if (rc != MCP2515_OK)
        {
            return rc;
        }
        if (rx[2] & (SIDL_IDE | SIDL_SRR))
        {
            continue; /* extended or remote frames aren't part of the FlatSat protocol */
        }
        frame->id  = (uint16_t)(((uint16_t)rx[1] << 3) | (rx[2] >> 5));
        frame->dlc = (uint8_t)(rx[5] & 0x0F) > 8 ? 8 : (uint8_t)(rx[5] & 0x0F);
        memcpy(frame->data, &rx[6], 8);
        return 1;
    }
}

int mcp2515_error_counters(mcp2515_t *d, uint8_t *tec, uint8_t *rec)
{
    uint8_t regs[2];
    int     rc = read_regs(d, MCP2515_REG_TEC, regs, 2);

    *tec = rc == MCP2515_OK ? regs[0] : 0;
    *rec = rc == MCP2515_OK ? regs[1] : 0;
    return rc;
}

int mcp2515_service(mcp2515_t *d)
{
    uint8_t eflg;
    int     rc = read_regs(d, MCP2515_REG_EFLG, &eflg, 1);

    if (rc != MCP2515_OK)
    {
        return rc;
    }
    if (eflg & EFLG_RXOVR)
    {
        d->rx_overflows += ((eflg & 0x40) ? 1u : 0u) + ((eflg & 0x80) ? 1u : 0u);
        rc = bit_modify(d, MCP2515_REG_EFLG, EFLG_RXOVR, 0);
    }
    /* Bus-off: the controller rejoins by itself after 128 x 11 recessive bits; it is counted once per event */
    if ((eflg & EFLG_TXBO) && !d->was_bus_off)
    {
        d->bus_off++;
    }
    d->was_bus_off = (eflg & EFLG_TXBO) != 0;
    return rc;
}

/*
** Register-level model of the MCP2515 (see fake_mcp2515.h), from datasheet DS20001801
*/
#include "fake_mcp2515.h"

#include <string.h>

#include "mcp2515.h"

uint8_t        fake_mcp2515_regs[128];
int            fake_mcp2515_present = 1;
int            fake_mcp2515_tx_hold;
unsigned       fake_mcp2515_sent_count;
fs_can_frame_t fake_mcp2515_sent[16];

#define R fake_mcp2515_regs
#define CANINTF_RX0IF 0x01
#define CANINTF_RX1IF 0x02
#define TXREQ         0x08 /* TXBnCTRL */
#define TXB(n)        (0x30 + 0x10 * (n)) /* TXBnCTRL; SIDH follows at +1 */

void fake_mcp2515_reset(void)
{
    memset(R, 0, sizeof(R));
    R[MCP2515_REG_CANSTAT] = 0x80; /* configuration mode */
    R[MCP2515_REG_CANCTRL] = 0x87;
}

/* Side effects of a register write */
static void wrote(uint8_t addr)
{
    if (addr == MCP2515_REG_CANCTRL)
    {
        R[MCP2515_REG_CANSTAT] = (uint8_t)((R[MCP2515_REG_CANSTAT] & 0x1F) | (R[MCP2515_REG_CANCTRL] & 0xE0));
    }
}

static void transmit(unsigned n)
{
    const uint8_t *b = &R[TXB(n) + 1]; /* SIDH, SIDL, EID8, EID0, DLC, D0-7 */
    fs_can_frame_t f;

    if ((R[MCP2515_REG_CANSTAT] & MCP2515_MODE_MASK) != MCP2515_MODE_NORMAL || fake_mcp2515_tx_hold)
    {
        return;
    }
    memset(&f, 0, sizeof(f));
    f.id  = (uint16_t)((b[0] << 3) | (b[1] >> 5));
    f.dlc = (uint8_t)(b[4] & 0x0F);
    memcpy(f.data, &b[5], 8);
    if (fake_mcp2515_sent_count < 16)
    {
        fake_mcp2515_sent[fake_mcp2515_sent_count++] = f;
    }
    R[TXB(n)] &= (uint8_t)~TXREQ;
    R[MCP2515_REG_CANINTF] |= (uint8_t)(0x04 << n); /* TXnIF */
}

void fake_mcp2515_complete_tx(void)
{
    unsigned n;
    int      hold = fake_mcp2515_tx_hold;

    fake_mcp2515_tx_hold = 0;
    for (n = 0; n < 3; n++)
    {
        if (R[TXB(n)] & TXREQ)
        {
            transmit(n);
        }
    }
    fake_mcp2515_tx_hold = hold;
}

int fake_mcp2515_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    uint8_t  scratch[32];
    uint8_t  op = tx[0];
    size_t   i;
    unsigned n;

    (void)ctx;
    if (rx == NULL)
    {
        rx = scratch;
    }
    memset(rx, 0xFF, len);
    if (!fake_mcp2515_present)
    {
        return 0;
    }

    if (op == MCP2515_CMD_RESET)
    {
        fake_mcp2515_reset();
    }
    else if (op == MCP2515_CMD_READ)
    {
        for (i = 2; i < len; i++)
        {
            rx[i] = R[(tx[1] + i - 2) & 0x7F];
        }
    }
    else if (op == MCP2515_CMD_WRITE)
    {
        for (i = 2; i < len; i++)
        {
            uint8_t addr = (uint8_t)((tx[1] + i - 2) & 0x7F);
            R[addr]      = tx[i];
            wrote(addr);
        }
    }
    else if (op == MCP2515_CMD_BITMOD && len == 4)
    {
        R[tx[1] & 0x7F] = (uint8_t)((R[tx[1] & 0x7F] & ~tx[2]) | (tx[3] & tx[2]));
        wrote(tx[1] & 0x7F);
    }
    else if (op == MCP2515_CMD_STATUS)
    {
        uint8_t intf = R[MCP2515_REG_CANINTF];
        uint8_t s    = (uint8_t)((intf & 0x01) | ((intf & 0x02)) | ((R[TXB(0)] & TXREQ) ? 0x04 : 0) |
                              ((intf & 0x04) ? 0x08 : 0) | ((R[TXB(1)] & TXREQ) ? 0x10 : 0) |
                              ((intf & 0x08) ? 0x20 : 0) | ((R[TXB(2)] & TXREQ) ? 0x40 : 0) | ((intf & 0x10) ? 0x80 : 0));
        for (i = 1; i < len; i++)
        {
            rx[i] = s;
        }
    }
    else if ((op & 0xF8) == MCP2515_CMD_LOAD_TX && (op & 0x07) <= 5)
    {
        uint8_t start = (uint8_t)(TXB((op & 0x07) / 2) + ((op & 1) ? 6 : 1)); /* SIDH, or D0 */
        for (i = 1; i < len; i++)
        {
            R[(start + i - 1) & 0x7F] = tx[i];
        }
    }
    else if ((op & 0xF8) == MCP2515_CMD_RTS)
    {
        for (n = 0; n < 3; n++)
        {
            if (op & (1u << n))
            {
                R[TXB(n)] |= TXREQ;
                transmit(n);
            }
        }
    }
    else if ((op & 0xF9) == MCP2515_CMD_READ_RX)
    {
        unsigned buf   = (op & 0x04) ? 1u : 0u;
        uint8_t  start = (uint8_t)(0x61 + 0x10 * buf + ((op & 0x02) ? 5 : 0)); /* SIDH, or D0 */
        for (i = 1; i < len; i++)
        {
            rx[i] = R[(start + i - 1) & 0x7F];
        }
        R[MCP2515_REG_CANINTF] &= (uint8_t)~(buf ? CANINTF_RX1IF : CANINTF_RX0IF); /* cleared as CS rises */
    }
    return 0;
}

void fake_mcp2515_receive(uint32_t id, int extended, int remote, uint8_t dlc, const uint8_t *data)
{
    uint8_t *b;
    unsigned buf;

    if (!(R[MCP2515_REG_CANINTF] & CANINTF_RX0IF))
    {
        buf = 0;
    }
    else if ((R[MCP2515_REG_RXB0CTRL] & 0x04) && !(R[MCP2515_REG_CANINTF] & CANINTF_RX1IF))
    {
        buf = 1; /* rollover */
    }
    else
    {
        R[MCP2515_REG_EFLG] |= (R[MCP2515_REG_RXB0CTRL] & 0x04) ? 0x80 : 0x40; /* RX1OVR, or RX0OVR */
        return;
    }
    b = &R[0x61 + 0x10 * buf];
    if (extended)
    {
        b[0] = (uint8_t)(id >> 21);
        b[1] = (uint8_t)(((id >> 13) & 0xE0) | 0x08 | ((id >> 16) & 0x03));
        b[2] = (uint8_t)(id >> 8);
        b[3] = (uint8_t)id;
    }
    else
    {
        b[0] = (uint8_t)(id >> 3);
        b[1] = (uint8_t)(((id & 0x07) << 5) | (remote ? 0x10 : 0));
        b[2] = b[3] = 0;
    }
    b[4] = (uint8_t)(dlc | ((extended && remote) ? 0x40 : 0));
    memset(&b[5], 0, 8);
    if (dlc > 0)
    {
        memcpy(&b[5], data, dlc > 8 ? 8 : dlc); /* a remote frame comes with no data */
    }
    R[MCP2515_REG_CANINTF] |= buf ? CANINTF_RX1IF : CANINTF_RX0IF;
}

/*
** Semtech SX1262 LoRa transceiver (see sx1262.h)
*/
#include "sx1262.h"

#include <string.h>

#include "fs_hal.h"

#define BUSY_POLLS     300      /* x 100 us: calibration after a reset takes about 3.5 ms */
#define STANDBY_RC     0x00
#define TCXO_1V7       0x01
#define TCXO_DELAY     (5 * 64) /* 5 ms in 15.625 us steps */
#define CALIBRATE_ALL  0x7F
#define REGULATOR_LDO  0x00     /* works on any board; DC-DC needs the inductor fitted */
#define PACKET_LORA    0x01
#define RX_CONTINUOUS  0xFFFFFFu
#define TX_TIMEOUT     64000u   /* 1 s in 15.625 us steps; an SF7 frame of 255 bytes takes about 0.4 s */
#define IRQS (SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_HEADER_ERR | SX1262_IRQ_CRC_ERR | SX1262_IRQ_TIMEOUT)

/* One radio per board: the frame-sized transfer buffer is static rather than on the stack (NCR-023) */
static uint8_t io_tx[4 + 256];
static uint8_t io_rx[4 + 256];

static int wait_ready(sx1262_t *r)
{
    int i;

    for (i = 0; i < BUSY_POLLS; i++)
    {
        if (!r->busy(r->ctx))
        {
            return SX1262_OK;
        }
        fs_hal_sleep_us(100);
    }
    return SX1262_ERR_BUSY;
}

/* A command with parameters, and any reply: the reply bytes follow the opcode (and its parameters) */
static int command(sx1262_t *r, const uint8_t *tx, uint8_t *rx, size_t len)
{
    int rc = wait_ready(r);

    if (rc != SX1262_OK)
    {
        return rc;
    }
    return r->xfer(r->ctx, tx, rx, len) == 0 ? SX1262_OK : SX1262_ERR_SPI;
}

static int cmd1(sx1262_t *r, uint8_t op, uint8_t a)
{
    const uint8_t tx[2] = {op, a};
    return command(r, tx, NULL, sizeof(tx));
}

static int write_reg(sx1262_t *r, uint16_t addr, const uint8_t *data, size_t n)
{
    io_tx[0] = SX1262_CMD_WRITE_REGISTER;
    io_tx[1] = (uint8_t)(addr >> 8);
    io_tx[2] = (uint8_t)addr;
    memcpy(&io_tx[3], data, n);
    return command(r, io_tx, NULL, 3 + n);
}

static int read_reg(sx1262_t *r, uint16_t addr, uint8_t *data, size_t n)
{
    int rc;

    memset(io_tx, 0, 4 + n);
    io_tx[0] = SX1262_CMD_READ_REGISTER;
    io_tx[1] = (uint8_t)(addr >> 8);
    io_tx[2] = (uint8_t)addr;
    rc       = command(r, io_tx, io_rx, 4 + n); /* opcode, address, status, then the data */
    memcpy(data, &io_rx[4], n);
    return rc;
}

static int set_packet_length(sx1262_t *r, uint8_t len)
{
    /* Preamble 8 symbols, explicit header, payload length (the maximum when listening), CRC on, standard IQ */
    const uint8_t tx[7] = {SX1262_CMD_SET_PACKET_PARAMS, 0x00, 0x08, 0x00, len, 0x01, 0x00};
    return command(r, tx, NULL, sizeof(tx));
}

static int clear_irq(sx1262_t *r, uint16_t mask)
{
    const uint8_t tx[3] = {SX1262_CMD_CLEAR_IRQ, (uint8_t)(mask >> 8), (uint8_t)mask};
    return command(r, tx, NULL, sizeof(tx));
}

static int listen(sx1262_t *r)
{
    const uint8_t tx[4] = {SX1262_CMD_SET_RX, (uint8_t)(RX_CONTINUOUS >> 16), (uint8_t)(RX_CONTINUOUS >> 8),
                           (uint8_t)RX_CONTINUOUS};
    int           rc    = set_packet_length(r, 0xFF);

    return rc != SX1262_OK ? rc : command(r, tx, NULL, sizeof(tx));
}

int sx1262_init(sx1262_t *r)
{
    static const uint8_t sync[2] = {0x14, 0x24};
    uint32_t             f       = (uint32_t)(((uint64_t)r->freq_hz << 25) / 32000000u);
    const uint8_t        tcxo[5] = {SX1262_CMD_SET_DIO3_TCXO, TCXO_1V7, (uint8_t)(TCXO_DELAY >> 16),
                                    (uint8_t)(TCXO_DELAY >> 8), (uint8_t)TCXO_DELAY};
    const uint8_t        freq[5] = {SX1262_CMD_SET_RF_FREQUENCY, (uint8_t)(f >> 24), (uint8_t)(f >> 16),
                                    (uint8_t)(f >> 8), (uint8_t)f};
    const uint8_t        image[3] = {SX1262_CMD_CALIBRATE_IMAGE, 0xD7, 0xDB}; /* the 863-870 MHz band */
    /* The high-power amplifier set up as Semtech's own driver does for every power, so the output is the set power */
    const uint8_t        pa[5]    = {SX1262_CMD_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01};
    const uint8_t        txp[3]   = {SX1262_CMD_SET_TX_PARAMS, (uint8_t)r->power_dbm, 0x04}; /* 200 us ramp */
    const uint8_t        base[3]  = {SX1262_CMD_SET_BUFFER_BASE, 0x00, 0x00};
    const uint8_t        mod[5]   = {SX1262_CMD_SET_MODULATION, 0x07, 0x04, 0x01, 0x00}; /* SF7, 125 kHz, 4/5 */
    const uint8_t        irq[9]   = {SX1262_CMD_SET_DIO_IRQ, (uint8_t)(IRQS >> 8), (uint8_t)IRQS,
                                     (uint8_t)(IRQS >> 8), (uint8_t)IRQS, 0, 0, 0, 0}; /* all on DIO1 */
    const uint8_t        clear_err[3] = {SX1262_CMD_CLEAR_ERRORS, 0, 0};
    uint8_t              check[2];
    uint8_t              clamp;
    int                  rc;

    r->transmitting  = 0;
    r->crc_errors    = 0;
    r->header_errors = 0;
    r->tx_timeouts   = 0;
    r->reset(r->ctx, 1);
    fs_hal_sleep_us(1000);
    r->reset(r->ctx, 0);
    fs_hal_sleep_us(5000);

    rc = cmd1(r, SX1262_CMD_SET_STANDBY, STANDBY_RC);
    if (rc == SX1262_OK)
    {
        rc = command(r, tcxo, NULL, sizeof(tcxo));
    }
    if (rc == SX1262_OK)
    {
        rc = cmd1(r, SX1262_CMD_CALIBRATE, CALIBRATE_ALL); /* with the TCXO running */
    }
    if (rc == SX1262_OK)
    {
        rc = cmd1(r, SX1262_CMD_SET_REGULATOR, REGULATOR_LDO);
    }
    if (rc == SX1262_OK)
    {
        rc = cmd1(r, SX1262_CMD_SET_DIO2_RF_SWITCH, 1);
    }
    if (rc == SX1262_OK)
    {
        rc = cmd1(r, SX1262_CMD_SET_PACKET_TYPE, PACKET_LORA);
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, freq, NULL, sizeof(freq));
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, image, NULL, sizeof(image));
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, pa, NULL, sizeof(pa));
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, txp, NULL, sizeof(txp));
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, base, NULL, sizeof(base));
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, mod, NULL, sizeof(mod));
    }
    if (rc == SX1262_OK)
    {
        rc = write_reg(r, SX1262_REG_SYNC_WORD, sync, sizeof(sync));
    }
    if (rc == SX1262_OK)
    {
        rc = read_reg(r, SX1262_REG_SYNC_WORD, check, sizeof(check));
    }
    if (rc != SX1262_OK)
    {
        return rc;
    }
    if (memcmp(check, sync, sizeof(sync)) != 0)
    {
        return SX1262_ERR_CHIP; /* a missing chip reads all ones or all zeros */
    }

    rc = read_reg(r, SX1262_REG_TX_CLAMP, &clamp, 1); /* errata 15.2 */
    if (rc == SX1262_OK)
    {
        clamp |= 0x1E;
        rc = write_reg(r, SX1262_REG_TX_CLAMP, &clamp, 1);
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, irq, NULL, sizeof(irq));
    }
    if (rc == SX1262_OK)
    {
        rc = clear_irq(r, 0xFFFF);
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, clear_err, NULL, sizeof(clear_err));
    }
    return rc != SX1262_OK ? rc : listen(r);
}

int sx1262_send(sx1262_t *r, const uint8_t *data, size_t len)
{
    const uint8_t tx[4] = {SX1262_CMD_SET_TX, (uint8_t)(TX_TIMEOUT >> 16), (uint8_t)(TX_TIMEOUT >> 8),
                           (uint8_t)TX_TIMEOUT};
    int           rc;

    if (len == 0 || len > 255)
    {
        return SX1262_ERR_ARG;
    }
    if (r->transmitting)
    {
        return SX1262_ERR_TX;
    }
    rc = cmd1(r, SX1262_CMD_SET_STANDBY, STANDBY_RC);
    if (rc == SX1262_OK)
    {
        rc = set_packet_length(r, (uint8_t)len);
    }
    if (rc == SX1262_OK)
    {
        io_tx[0] = SX1262_CMD_WRITE_BUFFER;
        io_tx[1] = 0x00; /* offset: the transmit base */
        memcpy(&io_tx[2], data, len);
        rc = command(r, io_tx, NULL, 2 + len);
    }
    if (rc == SX1262_OK)
    {
        rc = clear_irq(r, 0xFFFF);
    }
    if (rc == SX1262_OK)
    {
        rc = command(r, tx, NULL, sizeof(tx));
    }
    if (rc == SX1262_OK)
    {
        r->transmitting = 1;
    }
    return rc;
}

int sx1262_set_power(sx1262_t *r, int8_t dbm)
{
    const uint8_t txp[3] = {SX1262_CMD_SET_TX_PARAMS, (uint8_t)dbm, 0x04};
    int           rc;

    if (dbm < -9 || dbm > 22)
    {
        return SX1262_ERR_ARG;
    }
    rc = command(r, txp, NULL, sizeof(txp));
    if (rc == SX1262_OK)
    {
        r->power_dbm = dbm;
    }
    return rc;
}

int sx1262_poll(sx1262_t *r, uint8_t *buf, size_t max, size_t *len, int16_t *rssi_dbm, int8_t *snr_q4)
{
    const uint8_t get_irq[4] = {SX1262_CMD_GET_IRQ, 0, 0, 0};
    const uint8_t get_buf[4] = {SX1262_CMD_GET_RX_BUFFER, 0, 0, 0};
    const uint8_t get_pkt[5] = {SX1262_CMD_GET_PACKET_STATUS, 0, 0, 0, 0};
    uint8_t       reply[5];
    uint16_t      irq;
    size_t        n;
    int           rc = command(r, get_irq, reply, sizeof(get_irq)); /* opcode, status, IRQ MSB, LSB */

    if (rc != SX1262_OK)
    {
        return rc;
    }
    irq = (uint16_t)((reply[2] << 8) | reply[3]);
    if (irq == 0)
    {
        return SX1262_EVENT_NONE;
    }
    rc = clear_irq(r, irq);
    if (rc != SX1262_OK)
    {
        return rc;
    }

    if (irq & (SX1262_IRQ_TX_DONE | SX1262_IRQ_TIMEOUT))
    {
        int done = (irq & SX1262_IRQ_TX_DONE) != 0;
        if (!done && r->transmitting)
        {
            r->tx_timeouts++;
        }
        r->transmitting = 0;
        rc              = listen(r);
        return rc != SX1262_OK ? rc : (done ? SX1262_EVENT_TX_DONE : SX1262_EVENT_NONE);
    }
    if (irq & (SX1262_IRQ_CRC_ERR | SX1262_IRQ_HEADER_ERR))
    {
        r->crc_errors += (irq & SX1262_IRQ_CRC_ERR) ? 1u : 0u;
        r->header_errors += (irq & SX1262_IRQ_HEADER_ERR) ? 1u : 0u;
        return SX1262_EVENT_NONE;
    }
    if (!(irq & SX1262_IRQ_RX_DONE))
    {
        return SX1262_EVENT_NONE;
    }

    rc = command(r, get_buf, reply, sizeof(get_buf)); /* opcode, status, length, start */
    if (rc != SX1262_OK)
    {
        return rc;
    }
    n = reply[2] < max ? reply[2] : max;
    memset(io_tx, 0, 3 + n);
    io_tx[0] = SX1262_CMD_READ_BUFFER;
    io_tx[1] = reply[3];
    rc       = command(r, io_tx, io_rx, 3 + n); /* opcode, offset, status, then the data */
    if (rc != SX1262_OK)
    {
        return rc;
    }
    memcpy(buf, &io_rx[3], n);
    *len = n;

    rc = command(r, get_pkt, reply, sizeof(get_pkt)); /* opcode, status, RssiPkt, SnrPkt, SignalRssiPkt */
    if (rc != SX1262_OK)
    {
        return rc;
    }
    *rssi_dbm = (int16_t)(-(int16_t)reply[2] / 2);
    *snr_q4   = (int8_t)reply[3];
    return SX1262_EVENT_RX;
}

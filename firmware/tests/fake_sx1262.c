/*
** Command-level model of the SX1262 (see fake_sx1262.h), from datasheet DS_SX1261-2
*/
#include "fake_sx1262.h"

#include <string.h>

#include "sx1262.h"

fake_sx1262_t fake_sx1262;
#define F fake_sx1262

void fake_sx1262_power_on(void)
{
    int present = F.present;

    memset(&F, 0, sizeof(F));
    F.present                = present;
    F.mode                   = FAKE_SX1262_STANDBY;
    F.regs[0x0740]           = 0x34; /* the public LoRa sync word at power-on; the driver sets the private one */
    F.regs[0x0741]           = 0x44;
    F.regs[SX1262_REG_TX_CLAMP] = 0xC8;
}

void fake_sx1262_reset_line(void *ctx, int asserted)
{
    (void)ctx;
    if (asserted)
    {
        fake_sx1262_power_on();
    }
}

int fake_sx1262_busy(void *ctx)
{
    (void)ctx;
    return !F.present;
}

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void raise_irq(uint16_t bits)
{
    F.irq |= (uint16_t)(bits & F.irq_mask);
}

void fake_sx1262_tx_end(void)
{
    if (F.mode == FAKE_SX1262_TX)
    {
        memcpy(F.sent, F.buffer, F.packet[3]);
        F.sent_len = F.packet[3];
        F.sent_count++;
        F.mode = FAKE_SX1262_STANDBY; /* after TxDone the chip falls back to standby */
        raise_irq(SX1262_IRQ_TX_DONE);
    }
}

int fake_sx1262_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    uint8_t  scratch[300];
    uint8_t  op = tx[0];
    uint16_t addr;
    size_t   i;

    (void)ctx;
    if (rx == NULL)
    {
        rx = scratch;
    }
    memset(rx, F.present ? 0xA2 : 0xFF, len); /* status byte in every slot: standby RC, nothing pending */
    if (!F.present)
    {
        return 0;
    }

    switch (op)
    {
        case SX1262_CMD_SET_STANDBY: F.mode = FAKE_SX1262_STANDBY; break;
        case SX1262_CMD_SET_DIO3_TCXO: memcpy(F.tcxo, &tx[1], 4); break;
        case SX1262_CMD_CALIBRATE: F.calibrated = tx[1] == 0x7F; break;
        case SX1262_CMD_SET_REGULATOR: F.regulator = tx[1]; break;
        case SX1262_CMD_SET_DIO2_RF_SWITCH: F.rf_switch = tx[1]; break;
        case SX1262_CMD_SET_PACKET_TYPE: F.packet_type = tx[1]; break;
        case SX1262_CMD_SET_RF_FREQUENCY:
            F.freq_reg = ((uint32_t)tx[1] << 24) | ((uint32_t)tx[2] << 16) | ((uint32_t)tx[3] << 8) | tx[4];
            break;
        case SX1262_CMD_CALIBRATE_IMAGE: F.image_calibrated = tx[1] == 0xD7 && tx[2] == 0xDB; break;
        case SX1262_CMD_SET_PA_CONFIG: memcpy(F.pa, &tx[1], 4); break;
        case SX1262_CMD_SET_TX_PARAMS: memcpy(F.tx_params, &tx[1], 2); break;
        case SX1262_CMD_SET_BUFFER_BASE: break; /* both 0: the model's buffer starts there */
        case SX1262_CMD_SET_MODULATION: memcpy(F.modulation, &tx[1], 4); break;
        case SX1262_CMD_SET_PACKET_PARAMS: memcpy(F.packet, &tx[1], 6); break;
        case SX1262_CMD_SET_DIO_IRQ:
            F.irq_mask  = (uint16_t)((tx[1] << 8) | tx[2]);
            F.dio1_mask = (uint16_t)((tx[3] << 8) | tx[4]);
            break;
        case SX1262_CMD_CLEAR_IRQ: F.irq &= (uint16_t)~((tx[1] << 8) | tx[2]); break;
        case SX1262_CMD_CLEAR_ERRORS: break;
        case SX1262_CMD_GET_IRQ:
            rx[2] = (uint8_t)(F.irq >> 8);
            rx[3] = (uint8_t)F.irq;
            break;
        case SX1262_CMD_GET_RX_BUFFER:
            rx[2] = F.rx_len;
            rx[3] = F.rx_start;
            break;
        case SX1262_CMD_GET_PACKET_STATUS:
            rx[2] = F.rssi_pkt;
            rx[3] = F.snr_pkt;
            rx[4] = F.rssi_pkt;
            break;
        case SX1262_CMD_WRITE_REGISTER:
            addr = (uint16_t)((tx[1] << 8) | tx[2]);
            for (i = 3; i < len && addr + i - 3 < sizeof(F.regs); i++)
            {
                F.regs[addr + i - 3] = tx[i];
            }
            break;
        case SX1262_CMD_READ_REGISTER:
            addr = (uint16_t)((tx[1] << 8) | tx[2]);
            for (i = 4; i < len && addr + i - 4 < sizeof(F.regs); i++)
            {
                rx[i] = F.regs[addr + i - 4];
            }
            break;
        case SX1262_CMD_WRITE_BUFFER:
            for (i = 2; i < len; i++)
            {
                F.buffer[(uint8_t)(tx[1] + i - 2)] = tx[i];
            }
            break;
        case SX1262_CMD_READ_BUFFER:
            for (i = 3; i < len; i++)
            {
                rx[i] = F.buffer[(uint8_t)(tx[1] + i - 3)];
            }
            break;
        case SX1262_CMD_SET_RX:
            F.mode       = FAKE_SX1262_RX;
            F.rx_timeout = be24(&tx[1]);
            break;
        case SX1262_CMD_SET_TX:
            F.mode = FAKE_SX1262_TX;
            if (!F.tx_hold)
            {
                fake_sx1262_tx_end();
            }
            break;
        default: break;
    }
    return 0;
}

void fake_sx1262_air(const uint8_t *data, uint8_t len, int rssi_dbm, int snr_q4, int crc_error)
{
    if (F.mode != FAKE_SX1262_RX)
    {
        return; /* not listening: the frame is missed */
    }
    memcpy(F.buffer, data, len);
    F.rx_len   = len;
    F.rx_start = 0;
    F.rssi_pkt = (uint8_t)(-rssi_dbm * 2);
    F.snr_pkt  = (uint8_t)(int8_t)snr_q4;
    raise_irq((uint16_t)(SX1262_IRQ_RX_DONE | (crc_error ? SX1262_IRQ_CRC_ERR : 0)));
}

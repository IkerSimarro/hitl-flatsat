/*
** Command-level model of the SX1262 (tests/test_drivers.c): the commands the driver uses, the parameters they
** set, the data buffer, the IRQ flags, listening and transmitting, and frames arriving from the air.
*/
#ifndef FAKE_SX1262_H
#define FAKE_SX1262_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int      present;       /* 0: MISO floats high, BUSY reads high */
    int      tx_hold;       /* 1: a transmission doesn't end until fake_sx1262_tx_end() */
    int      mode;          /* FAKE_SX1262_* */
    uint32_t rx_timeout;
    uint8_t  tcxo[4];       /* voltage, delay (3 bytes) */
    int      rf_switch;
    uint8_t  packet_type;
    uint32_t freq_reg;
    uint8_t  modulation[4];
    uint8_t  packet[6];
    uint8_t  pa[4];
    uint8_t  tx_params[2];
    uint8_t  regulator;
    int      calibrated, image_calibrated;
    uint16_t irq_mask, dio1_mask, irq;
    uint8_t  regs[0x0A00];
    uint8_t  buffer[256];
    uint8_t  rx_len, rx_start;
    uint8_t  rssi_pkt, snr_pkt;
    uint8_t  sent[256];
    size_t   sent_len;
    unsigned sent_count;
} fake_sx1262_t;

enum { FAKE_SX1262_SLEEP, FAKE_SX1262_STANDBY, FAKE_SX1262_RX, FAKE_SX1262_TX };

extern fake_sx1262_t fake_sx1262;

void fake_sx1262_power_on(void);
int  fake_sx1262_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
int  fake_sx1262_busy(void *ctx);
void fake_sx1262_reset_line(void *ctx, int asserted);
void fake_sx1262_tx_end(void);
/* A frame from the air: received only while listening; crc_error marks it as corrupted */
void fake_sx1262_air(const uint8_t *data, uint8_t len, int rssi_dbm, int snr_q4, int crc_error);

#endif /* FAKE_SX1262_H */

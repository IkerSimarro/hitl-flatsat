/*
** Semtech SX1262 LoRa transceiver (Waveshare Pico-LoRa-SX1262-868M: the OBC's radio and the ground modem, ICD 7)
**
** Portable: the chip is reached through callbacks for SPI, its BUSY line and its reset line, so the same code
** runs on the Pico (SPI1, GP10-GP12; CS GP3, BUSY GP2, RESET GP15) and in the unit tests against a command-level
** model of the chip (tests/fake_sx1262.c).
**
** Board facts, confirmed against a working port of this board (BNNorman/Waveshare-Pico-LoRa-SX1262-868M): the
** module's TCXO is powered from DIO3 at 1.7 V and needs 5 ms to start, and DIO2 drives its RF switch.
**
** ICD 7.1: 869.525 MHz; LoRa SF7, 125 kHz, CR 4/5, 8-symbol preamble, explicit header, CRC on; private network
** sync word 0x1424; 2 dBm. The radio listens continuously between transmissions; received frames and the end of
** a transmission are polled (sx1262_poll), from the main loop.
*/
#ifndef SX1262_H
#define SX1262_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* One SPI transaction with chip select held low for all len bytes; rx may be NULL. 0 on success. */
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    int (*busy)(void *ctx);                 /* the BUSY line: 1 while the chip can't take a command */
    void (*reset)(void *ctx, int asserted); /* drives NRESET low while asserted */
    void *ctx;

    /* Set before sx1262_init */
    uint32_t freq_hz;
    int8_t   power_dbm;

    /* Maintained by the driver */
    int      transmitting;
    uint32_t crc_errors;
    uint32_t header_errors;
    uint32_t tx_timeouts;
} sx1262_t;

#define SX1262_OK       0
#define SX1262_ERR_SPI  (-1) /* the transfer failed */
#define SX1262_ERR_BUSY (-2) /* BUSY stayed high: the chip is stuck, absent, or still in reset */
#define SX1262_ERR_CHIP (-3) /* no SX1262 answering as one should */
#define SX1262_ERR_ARG  (-4) /* a frame that can't be sent (empty, or longer than 255 bytes) */
#define SX1262_ERR_TX   (-5) /* a transmission is still under way */

#define SX1262_EVENT_NONE    0
#define SX1262_EVENT_RX      1 /* a frame arrived (its length is returned) */
#define SX1262_EVENT_TX_DONE 2

/* Resets and configures the radio for ICD 7.1, then listens */
int sx1262_init(sx1262_t *r);
/* Starts a transmission; the radio returns to listening when it ends (sx1262_poll) */
int sx1262_send(sx1262_t *r, const uint8_t *data, size_t len);
/* Changes the transmit power (-9..22 dBm on the SX1262's high-power amplifier) */
int sx1262_set_power(sx1262_t *r, int8_t dbm);
/*
** Handles what the radio has to report: SX1262_EVENT_NONE, SX1262_EVENT_TX_DONE, or a received frame, copied to
** buf (*len its length, *rssi_dbm, *snr_q4 the SNR in 0.25 dB steps) and SX1262_EVENT_RX. Frames with a CRC or
** header error are counted and dropped. < 0 on error.
*/
int sx1262_poll(sx1262_t *r, uint8_t *buf, size_t max, size_t *len, int16_t *rssi_dbm, int8_t *snr_q4);

/* Commands, registers and IRQ bits (datasheet DS_SX1261-2), for the driver and the chip model */
#define SX1262_CMD_SET_STANDBY        0x80
#define SX1262_CMD_SET_RX             0x82
#define SX1262_CMD_SET_TX             0x83
#define SX1262_CMD_SET_RF_FREQUENCY   0x86
#define SX1262_CMD_SET_PACKET_TYPE    0x8A
#define SX1262_CMD_SET_MODULATION     0x8B
#define SX1262_CMD_SET_PACKET_PARAMS  0x8C
#define SX1262_CMD_SET_TX_PARAMS      0x8E
#define SX1262_CMD_SET_BUFFER_BASE    0x8F
#define SX1262_CMD_SET_PA_CONFIG      0x95
#define SX1262_CMD_SET_REGULATOR      0x96
#define SX1262_CMD_SET_DIO3_TCXO      0x97
#define SX1262_CMD_CALIBRATE_IMAGE    0x98
#define SX1262_CMD_CALIBRATE          0x89
#define SX1262_CMD_SET_DIO2_RF_SWITCH 0x9D
#define SX1262_CMD_SET_DIO_IRQ        0x08
#define SX1262_CMD_CLEAR_IRQ          0x02
#define SX1262_CMD_GET_IRQ            0x12
#define SX1262_CMD_GET_RX_BUFFER      0x13
#define SX1262_CMD_GET_PACKET_STATUS  0x14
#define SX1262_CMD_WRITE_REGISTER     0x0D
#define SX1262_CMD_READ_REGISTER      0x1D
#define SX1262_CMD_WRITE_BUFFER       0x0E
#define SX1262_CMD_READ_BUFFER        0x1E
#define SX1262_CMD_CLEAR_ERRORS       0x07

#define SX1262_REG_SYNC_WORD 0x0740 /* MSB, LSB at 0x0741 */
#define SX1262_REG_TX_CLAMP  0x08D8 /* errata 15.2: set bits 4:1 for antenna mismatch resistance */

#define SX1262_IRQ_TX_DONE    0x0001
#define SX1262_IRQ_RX_DONE    0x0002
#define SX1262_IRQ_HEADER_ERR 0x0020
#define SX1262_IRQ_CRC_ERR    0x0040
#define SX1262_IRQ_TIMEOUT    0x0200

#ifdef __cplusplus
}
#endif

#endif /* SX1262_H */

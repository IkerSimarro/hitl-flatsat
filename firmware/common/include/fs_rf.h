/*
** RF frames (ICD 7.2) and LoRa airtime (ICD 7.3)
**
**   header u8 (version 7-6, frame type 5-4) | spacecraft ID u8 | frame counter u16 BE | space packet | CRC-16 BE
**
** Portable C, no OS dependencies: the same code runs in the OBC, on the Pico and in the unit tests; the ground
** station software has a Python twin (ground/flatsat_rf.py) checked against the same test vectors.
*/
#ifndef FS_RF_H
#define FS_RF_H

#include <stddef.h>
#include <stdint.h>

#define FS_RF_SCID      0x01
#define FS_RF_VERSION   0
#define FS_RF_HDR_LEN   4
#define FS_RF_CRC_LEN   2
#define FS_RF_MAX_FRAME 255
#define FS_RF_MAX_DATA  (FS_RF_MAX_FRAME - FS_RF_HDR_LEN - FS_RF_CRC_LEN) /* 249: ICD RF_MAX_PACKET */

/* fs_rf_parse results */
#define FS_RF_OK          0
#define FS_RF_ERR_LENGTH  (-1) /* shorter than header + CRC, or longer than a LoRa packet */
#define FS_RF_ERR_CRC     (-2)
#define FS_RF_ERR_ID      (-3) /* another spacecraft, or an unknown version */

typedef struct
{
    uint8_t        type;    /* flatsat_rf_frame_type_t */
    uint16_t       counter;
    const uint8_t *data;    /* the space packet (none in a HAIL frame) */
    size_t         data_len;
} fs_rf_frame_t;

/* Builds a frame into out; returns its length, or 0 if it doesn't fit */
size_t fs_rf_build(uint8_t *out, size_t out_max, uint8_t type, uint16_t counter, const uint8_t *data, size_t len);

/* Checks and splits a received frame; the data pointer points into the frame */
int fs_rf_parse(const uint8_t *frame, size_t len, fs_rf_frame_t *out);

/* Time on air of a LoRa packet of len bytes, in microseconds: SF7, 125 kHz, coding rate 4/5, 8-symbol
** preamble, explicit header, CRC on (ICD 7.1), per Semtech AN1200.13 */
uint32_t fs_rf_airtime_us(size_t len);

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), as in the CCSDS frame error control field */
uint16_t fs_rf_crc16(const uint8_t *data, size_t len);

#endif /* FS_RF_H */

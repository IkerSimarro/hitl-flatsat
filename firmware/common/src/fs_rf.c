/*
** RF frames and LoRa airtime (see fs_rf.h)
*/
#include "fs_rf.h"

#include <string.h>

uint16_t fs_rf_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t   i;
    int      b;

    for (i = 0; i < len; i++)
    {
        crc ^= (uint16_t)(data[i] << 8);
        for (b = 0; b < 8; b++)
        {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t fs_rf_build(uint8_t *out, size_t out_max, uint8_t type, uint16_t counter, const uint8_t *data, size_t len)
{
    size_t   total = FS_RF_HDR_LEN + len + FS_RF_CRC_LEN;
    uint16_t crc;

    if (len > FS_RF_MAX_DATA || total > out_max || (len > 0 && data == NULL))
    {
        return 0;
    }
    out[0] = (uint8_t)((FS_RF_VERSION << 6) | ((type & 0x3) << 4));
    out[1] = FS_RF_SCID;
    out[2] = (uint8_t)(counter >> 8);
    out[3] = (uint8_t)counter;
    if (len > 0)
    {
        memcpy(&out[FS_RF_HDR_LEN], data, len);
    }
    crc                         = fs_rf_crc16(out, FS_RF_HDR_LEN + len);
    out[FS_RF_HDR_LEN + len]     = (uint8_t)(crc >> 8);
    out[FS_RF_HDR_LEN + len + 1] = (uint8_t)crc;
    return total;
}

int fs_rf_parse(const uint8_t *frame, size_t len, fs_rf_frame_t *out)
{
    uint16_t crc;

    if (len < FS_RF_HDR_LEN + FS_RF_CRC_LEN || len > FS_RF_MAX_FRAME)
    {
        return FS_RF_ERR_LENGTH;
    }
    crc = (uint16_t)((frame[len - 2] << 8) | frame[len - 1]);
    if (crc != fs_rf_crc16(frame, len - FS_RF_CRC_LEN))
    {
        return FS_RF_ERR_CRC;
    }
    if ((frame[0] >> 6) != FS_RF_VERSION || (frame[0] & 0x0F) != 0 || frame[1] != FS_RF_SCID)
    {
        return FS_RF_ERR_ID;
    }
    out->type     = (uint8_t)((frame[0] >> 4) & 0x3);
    out->counter  = (uint16_t)((frame[2] << 8) | frame[3]);
    out->data     = &frame[FS_RF_HDR_LEN];
    out->data_len = len - FS_RF_HDR_LEN - FS_RF_CRC_LEN;
    return FS_RF_OK;
}

uint32_t fs_rf_airtime_us(size_t len)
{
    /* SF 7, BW 125 kHz: symbol time 2^7 / 125 kHz = 1024 us. Low data rate optimisation off (SF < 11). */
    const int32_t sf = 7, cr = 1, crc_on = 1, implicit = 0, preamble = 8;
    const uint32_t t_sym_us = 1024u;
    int32_t        num      = 8 * (int32_t)len - 4 * sf + 28 + 16 * crc_on - 20 * implicit;
    int32_t        den      = 4 * sf;
    int32_t        n_payload = 8;

    if (num > 0)
    {
        n_payload += ((num + den - 1) / den) * (cr + 4);
    }
    /* preamble: (n + 4.25) symbols */
    return (uint32_t)(preamble * t_sym_us + (17u * t_sym_us) / 4u + (uint32_t)n_payload * t_sym_us);
}

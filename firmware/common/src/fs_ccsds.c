/*
** CCSDS space packets with cFS-compatible secondary headers (ICD section 3)
*/
#include "fs_ccsds.h"

#include <string.h>

#define CCSDS_PRI_HDR_LEN   6
#define CCSDS_TYPE_TC       0x1000u /* packet type bit in the stream ID */
#define CCSDS_SEC_HDR_FLAG  0x0800u
#define CCSDS_SEQ_UNSEGMENTED 0xC000u
#define CCSDS_SEQ_COUNT_MASK  0x3FFFu

/* One sequence counter per telemetry MID. The FlatSat has few packets, so a small table is enough. */
#define FS_MAX_TLM_MIDS 32

typedef struct
{
    uint16_t mid;
    uint16_t count;
} fs_seq_entry_t;

static fs_seq_entry_t seq_table[FS_MAX_TLM_MIDS];
static size_t         seq_used;

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint16_t next_sequence(uint16_t mid)
{
    size_t   i;
    uint16_t count;

    for (i = 0; i < seq_used; i++)
    {
        if (seq_table[i].mid == mid)
        {
            break;
        }
    }
    if (i == seq_used)
    {
        if (seq_used == FS_MAX_TLM_MIDS)
        {
            return 0; /* table full: still a valid packet, just without a running count */
        }
        seq_table[seq_used].mid   = mid;
        seq_table[seq_used].count = 0;
        seq_used++;
    }
    count              = seq_table[i].count;
    seq_table[i].count = (uint16_t)((count + 1) & CCSDS_SEQ_COUNT_MASK);
    return count;
}

void fs_tlm_reset_sequences(void)
{
    seq_used = 0;
}

size_t fs_tlm_build(uint8_t *buf, size_t buf_size, uint16_t mid, uint32_t seconds, uint16_t subseconds,
                    const void *payload, size_t payload_len)
{
    size_t total = FLATSAT_TLM_HDR_LEN + payload_len;

    if (buf == NULL || total > buf_size || total > 0xFFFF + 7u)
    {
        return 0;
    }

    put_be16(&buf[0], mid);
    put_be16(&buf[2], (uint16_t)(CCSDS_SEQ_UNSEGMENTED | next_sequence(mid)));
    put_be16(&buf[4], (uint16_t)(total - 7));
    put_be32(&buf[6], seconds);
    put_be16(&buf[10], subseconds);
    memset(&buf[12], 0, 4);
    if (payload_len > 0)
    {
        memcpy(&buf[FLATSAT_TLM_HDR_LEN], payload, payload_len);
    }
    return total;
}

uint16_t fs_pkt_mid(const uint8_t *pkt, size_t len)
{
    return (pkt != NULL && len >= CCSDS_PRI_HDR_LEN) ? get_be16(pkt) : 0;
}

fs_cmd_status_t fs_cmd_parse(const uint8_t *pkt, size_t len, fs_cmd_t *cmd)
{
    uint16_t mid;
    uint8_t  x = 0;
    size_t   i;

    if (pkt == NULL || len < FLATSAT_CMD_HDR_LEN)
    {
        return FS_CMD_TOO_SHORT;
    }

    mid = get_be16(pkt);
    if ((mid & (CCSDS_TYPE_TC | CCSDS_SEC_HDR_FLAG)) != (CCSDS_TYPE_TC | CCSDS_SEC_HDR_FLAG))
    {
        return FS_CMD_NOT_COMMAND;
    }
    if ((size_t)get_be16(&pkt[4]) + 7 != len)
    {
        return FS_CMD_LENGTH_MISMATCH;
    }

    /* cFS rule: the checksum byte makes the XOR of the whole packet 0xFF */
    for (i = 0; i < len; i++)
    {
        x ^= pkt[i];
    }
    if (x != 0xFF)
    {
        return FS_CMD_BAD_CHECKSUM;
    }

    cmd->mid      = mid;
    cmd->fc       = pkt[6];
    cmd->args     = &pkt[FLATSAT_CMD_HDR_LEN];
    cmd->args_len = len - FLATSAT_CMD_HDR_LEN;
    return FS_CMD_OK;
}

size_t fs_cmd_build(uint8_t *buf, size_t buf_size, uint16_t mid, uint8_t fc, const void *args, size_t args_len)
{
    size_t  total = FLATSAT_CMD_HDR_LEN + args_len;
    uint8_t x     = 0;
    size_t  i;

    if (buf == NULL || total > buf_size)
    {
        return 0;
    }

    put_be16(&buf[0], mid);
    put_be16(&buf[2], CCSDS_SEQ_UNSEGMENTED);
    put_be16(&buf[4], (uint16_t)(total - 7));
    buf[6] = fc;
    buf[7] = 0;
    if (args_len > 0)
    {
        memcpy(&buf[FLATSAT_CMD_HDR_LEN], args, args_len);
    }
    for (i = 0; i < total; i++)
    {
        x ^= buf[i];
    }
    buf[7] = (uint8_t)(x ^ 0xFF);
    return total;
}

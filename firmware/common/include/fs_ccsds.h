/*
** CCSDS space packets with cFS-compatible secondary headers (ICD section 3)
**
** Telemetry: 6-byte primary header, 6-byte CUC time, 4-byte spare, then the payload struct from
** flatsat_icd.h. Commands: 6-byte primary header, function code, checksum, then the arguments.
** Headers are big-endian; payloads are little-endian and copied straight from the generated structs.
*/
#ifndef FS_CCSDS_H
#define FS_CCSDS_H

#include <stddef.h>
#include <stdint.h>

#include "flatsat_icd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    FS_CMD_OK = 0,
    FS_CMD_TOO_SHORT,       /* shorter than the 8-byte command header */
    FS_CMD_NOT_COMMAND,     /* type bit or secondary-header flag not set */
    FS_CMD_LENGTH_MISMATCH, /* CCSDS length field disagrees with the received size */
    FS_CMD_BAD_CHECKSUM     /* XOR of all bytes is not 0xFF */
} fs_cmd_status_t;

typedef struct
{
    uint16_t       mid;
    uint8_t        fc;
    const uint8_t *args;
    size_t         args_len;
} fs_cmd_t;

/*
** Builds a telemetry packet in buf: header with the next sequence count for this MID and the given
** time, followed by payload_len bytes of payload. Returns the total length, or 0 if buf is too small.
*/
size_t fs_tlm_build(uint8_t *buf, size_t buf_size, uint16_t mid, uint32_t seconds, uint16_t subseconds,
                    const void *payload, size_t payload_len);

/* Checks a received command (ICD 3.2) and, if valid, fills cmd with its MID, function code and arguments */
fs_cmd_status_t fs_cmd_parse(const uint8_t *pkt, size_t len, fs_cmd_t *cmd);

/* Builds a command packet with a valid checksum; returns the total length or 0. Used by tests and the ground. */
size_t fs_cmd_build(uint8_t *buf, size_t buf_size, uint16_t mid, uint8_t fc, const void *args, size_t args_len);

/* MID of a telemetry packet, or 0 if the packet is too short */
uint16_t fs_pkt_mid(const uint8_t *pkt, size_t len);

/* Resets all telemetry sequence counters (tests and reboots) */
void fs_tlm_reset_sequences(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_CCSDS_H */

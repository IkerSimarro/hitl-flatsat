/*
** Helpers shared by the OBC device drivers
*/
#ifndef DEV_INTERNAL_H
#define DEV_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "fs_umbilical.h"
#include "obc_devices.h"

/* Bridge timeout for I2C/SPI/CAN transactions */
#define DEV_TXN_TIMEOUT_MS 100

/* Maps an umbilical transaction result to a DEV_ERR_* code */
int dev_map_umb(int rc);

/*
** Request/reply on a UART device: discards stale input, writes the request, then collects the reply.
** Returns once `expected` bytes have arrived (0 = unknown length) or the line has been quiet for
** quiet_ms after the first byte. Returns the number of bytes read, or a negative DEV_ERR_* code.
*/
int dev_uart_request(uint8_t bus, const uint8_t *req, size_t req_len, uint8_t *reply, size_t reply_max,
                     size_t expected, uint32_t first_byte_timeout_ms, uint32_t quiet_ms);

/* Big-endian readers */
static inline uint16_t dev_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t dev_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#endif /* DEV_INTERNAL_H */

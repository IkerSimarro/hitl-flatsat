/*
** Fake HAL for unit tests (see fake_hal.h)
**
** The fake bridge answers I2C/SPI/CAN transactions with rx_len bytes where byte i = first tx byte + i,
** so tests can check that responses are matched and copied correctly.
*/
#include "fake_hal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "flatsat_icd.h"
#include "fs_hal.h"
#include "hil_link.h"

uint64_t fake_now_us;
int      fake_bridge_silent;
uint8_t  fake_bridge_status;
uint8_t  fake_last_type;
uint8_t  fake_last_bus;
uint32_t fake_last_addr;
uint8_t  fake_last_payload[256];
size_t   fake_last_len;
unsigned fake_frames_from_fw;

fs_can_frame_t        fake_can_sent[FAKE_CAN_MAX];
unsigned              fake_can_sent_count;
static fs_can_frame_t can_rx_queue[FAKE_CAN_MAX];
static unsigned       can_rx_head;
static unsigned       can_rx_tail;

static hil_decoder_t dec;
static hil_frame_t   frame;
static hil_frame_t   reply;
static uint8_t       to_fw[16384];
static size_t        to_fw_len;
static size_t        to_fw_pos;

static void queue_frame(const hil_frame_t *f)
{
    uint8_t wire[HIL_ENCODED_MAX];
    size_t  n = hil_encode(f, wire, sizeof(wire));

    if (to_fw_pos == to_fw_len)
    {
        to_fw_pos = to_fw_len = 0;
    }
    if (n > 0 && to_fw_len + n <= sizeof(to_fw))
    {
        memcpy(&to_fw[to_fw_len], wire, n);
        to_fw_len += n;
    }
}

void fake_bridge_inject(uint8_t type, uint8_t bus, const uint8_t *payload, size_t len)
{
    memset(&reply, 0, sizeof(reply));
    reply.type = type;
    reply.bus  = bus;
    reply.len  = (uint16_t)len;
    memcpy(reply.payload, payload, len);
    queue_frame(&reply);
}

static void bridge_handle(const hil_frame_t *f)
{
    fake_frames_from_fw++;
    fake_last_type = f->type;
    fake_last_bus  = f->bus;
    fake_last_addr = f->addr;
    fake_last_len  = f->len < sizeof(fake_last_payload) ? f->len : sizeof(fake_last_payload);
    memcpy(fake_last_payload, f->payload, fake_last_len);

    if (fake_bridge_silent)
    {
        return;
    }
    if (f->type == HIL_I2C_TXN || f->type == HIL_SPI_TXN || f->type == HIL_CAN_TXN)
    {
        uint16_t rx_len = hil_get_u16(f->payload);
        uint16_t i;

        memset(&reply, 0, sizeof(reply));
        reply.type   = (uint8_t)(f->type + 1);
        reply.bus    = f->bus;
        reply.seq    = f->seq;
        reply.status = fake_bridge_status;
        reply.addr   = f->addr;
        if (fake_bridge_status == HIL_STATUS_OK)
        {
            uint8_t first = f->len > 2 ? f->payload[2] : 0;
            reply.len     = rx_len;
            for (i = 0; i < rx_len; i++)
            {
                reply.payload[i] = (uint8_t)(first + i);
            }
        }
        queue_frame(&reply);
    }
}

void fake_hal_reset(void)
{
    fake_now_us         = 1000;
    fake_bridge_silent  = 0;
    fake_bridge_status  = HIL_STATUS_OK;
    fake_last_type      = 0;
    fake_last_len       = 0;
    fake_frames_from_fw = 0;
    to_fw_len = to_fw_pos = 0;
    hil_decoder_init(&dec);
    fake_can_sent_count = 0;
    can_rx_head = can_rx_tail = 0;
}

/* ---- HAL ---- */

int fs_hal_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fake_hal_reset();
    return 0;
}

uint64_t fs_hal_time_us(void)
{
    return fake_now_us;
}

void fs_hal_sleep_us(uint32_t us)
{
    fake_now_us += us;
}

int fs_hal_umb_write(const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        if (hil_decoder_feed(&dec, data[i], &frame) == HIL_DECODE_FRAME)
        {
            bridge_handle(&frame);
        }
    }
    return (int)len;
}

int fs_hal_umb_read(uint8_t *buf, size_t max)
{
    size_t n = to_fw_len - to_fw_pos;

    if (n > max)
    {
        n = max;
    }
    memcpy(buf, &to_fw[to_fw_pos], n);
    to_fw_pos += n;
    return (int)n;
}

void fake_can_inject(uint16_t id, const void *data, uint8_t dlc)
{
    fs_can_frame_t *f = &can_rx_queue[can_rx_head % FAKE_CAN_MAX];

    f->id  = id;
    f->dlc = dlc;
    memset(f->data, 0, sizeof(f->data));
    memcpy(f->data, data, dlc);
    can_rx_head++;
}

int fs_hal_can_send(const fs_can_frame_t *f)
{
    if (fake_can_sent_count < FAKE_CAN_MAX)
    {
        fake_can_sent[fake_can_sent_count++] = *f;
    }
    return 0;
}

int fs_hal_can_recv(fs_can_frame_t *f)
{
    if (can_rx_tail == can_rx_head)
    {
        return 0;
    }
    *f = can_rx_queue[can_rx_tail % FAKE_CAN_MAX];
    can_rx_tail++;
    return 1;
}

void fs_hal_can_error_counters(uint8_t *tec, uint8_t *rec)
{
    *tec = 0;
    *rec = 0;
}

uint8_t fs_hal_reset_cause(void)
{
    return FLATSAT_RESET_CAUSE_POWER_ON;
}

int fake_reboots;

void fs_hal_reboot(void)
{
    fake_reboots++;
}

void fs_hal_watchdog_kick(void)
{
}

void fs_hal_check_power(void)
{
}

void fs_hal_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

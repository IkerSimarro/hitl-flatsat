/*
** Umbilical client (see fs_umbilical.h)
*/
#include "fs_umbilical.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fs_hal.h"
#include "hil_link.h"

/* UART receive buffering for the device sims on usart_N; only a few ports are used at once */
#define UART_SLOTS     8
#define UART_RING_SIZE 512
#define NO_BUS         0xFF

typedef struct
{
    uint8_t  bus;
    uint16_t head;
    uint16_t tail;
    uint8_t  data[UART_RING_SIZE];
} uart_ring_t;

static fs_umb_handlers_t handlers;
static fs_umb_stats_t    stats;
static hil_decoder_t     decoder;
static hil_frame_t       rx_frame;
static hil_frame_t       tx_frame;
static uint8_t           wire[HIL_ENCODED_MAX];
static uint8_t           txn_payload[HIL_MAX_PAYLOAD]; /* static: the Pico's default stack is only 2 KB */
static uart_ring_t       uart_rings[UART_SLOTS];

static uint8_t  next_seq;
static uint64_t last_rx_us;
static int      in_handler;

/* Response the current transaction is waiting for */
static struct
{
    int      active;
    uint8_t  type;
    uint8_t  seq;
    int      done;
    int      status;
    uint8_t *rx;
    size_t   rx_len;
} pending;

void fs_umb_init(const fs_umb_handlers_t *h)
{
    unsigned i;

    memset(&handlers, 0, sizeof(handlers));
    if (h != NULL)
    {
        handlers = *h;
    }
    memset(&stats, 0, sizeof(stats));
    memset(&pending, 0, sizeof(pending));
    hil_decoder_init(&decoder);
    for (i = 0; i < UART_SLOTS; i++)
    {
        uart_rings[i].bus  = NO_BUS;
        uart_rings[i].head = 0;
        uart_rings[i].tail = 0;
    }
    next_seq   = 0;
    last_rx_us = 0;
    in_handler = 0;
}

int fs_umb_link_up(void)
{
    return last_rx_us != 0 && fs_hal_time_us() - last_rx_us < FS_UMB_LINK_TIMEOUT_US;
}

const fs_umb_stats_t *fs_umb_stats(void)
{
    return &stats;
}

static int send_frame(uint8_t type, uint8_t bus, uint8_t seq, uint32_t addr, const void *payload, size_t len)
{
    size_t n;

    if (len > HIL_MAX_PAYLOAD)
    {
        return FS_UMB_TOO_LONG;
    }
    tx_frame.type   = type;
    tx_frame.bus    = bus;
    tx_frame.seq    = seq;
    tx_frame.status = 0;
    tx_frame.addr   = addr;
    tx_frame.len    = (uint16_t)len;
    if (len > 0)
    {
        memcpy(tx_frame.payload, payload, len);
    }

    n = hil_encode(&tx_frame, wire, sizeof(wire));
    if (n == 0 || fs_hal_umb_write(wire, n) < 0)
    {
        return FS_UMB_IO;
    }
    stats.tx_frames++;
    return FS_UMB_OK;
}

/* ---- UART rings ---- */

static uart_ring_t *find_ring(uint8_t bus)
{
    unsigned i;

    for (i = 0; i < UART_SLOTS; i++)
    {
        if (uart_rings[i].bus == bus)
        {
            return &uart_rings[i];
        }
    }
    return NULL;
}

static void ring_push(uart_ring_t *r, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        uint16_t next = (uint16_t)((r->head + 1) % UART_RING_SIZE);
        if (next == r->tail)
        {
            stats.uart_overflows++;
            return;
        }
        r->data[r->head] = data[i];
        r->head          = next;
    }
}

/* ---- Receive path ---- */

static void dispatch(const hil_frame_t *f)
{
    /* Response to the transaction in progress? */
    if (pending.active && !pending.done && f->type == pending.type && f->seq == pending.seq)
    {
        pending.status = f->status;
        if (f->status == HIL_STATUS_OK)
        {
            if (f->len != pending.rx_len)
            {
                pending.status = HIL_STATUS_BAD_REQ;
            }
            else if (f->len > 0)
            {
                memcpy(pending.rx, f->payload, f->len);
            }
        }
        pending.done = 1;
        return;
    }

    in_handler = 1;
    switch (f->type)
    {
        case HIL_CI_PKT:
            if (handlers.on_command)
            {
                handlers.on_command(f->payload, f->len);
            }
            break;

        case HIL_RF_RX:
            if (handlers.on_rf_frame)
            {
                handlers.on_rf_frame(f->payload, f->len);
            }
            break;

        case HIL_TIME:
            if (handlers.on_time && f->len == 6)
            {
                fs_time_t t;
                t.seconds = (uint32_t)f->payload[0] | ((uint32_t)f->payload[1] << 8) |
                            ((uint32_t)f->payload[2] << 16) | ((uint32_t)f->payload[3] << 24);
                t.subseconds = hil_get_u16(&f->payload[4]);
                handlers.on_time(t);
            }
            break;

        case HIL_UART_RX:
        {
            uart_ring_t *r = find_ring(f->bus);
            if (r != NULL)
            {
                ring_push(r, f->payload, f->len);
            }
            break;
        }

        default:
            /* Heartbeat echoes and late responses to timed-out transactions */
            break;
    }
    in_handler = 0;
}

void fs_umb_poll(void)
{
    uint8_t buf[256];
    int     n;
    int     i;

    while ((n = fs_hal_umb_read(buf, sizeof(buf))) > 0)
    {
        for (i = 0; i < n; i++)
        {
            hil_decode_result_t r = hil_decoder_feed(&decoder, buf[i], &rx_frame);
            if (r == HIL_DECODE_FRAME)
            {
                stats.rx_frames++;
                last_rx_us = fs_hal_time_us();
                dispatch(&rx_frame);
            }
            else if (r == HIL_DECODE_ERROR)
            {
                stats.decode_errors++;
            }
        }
    }
}

/* ---- Transactions ---- */

static int transact(uint8_t req_type, uint8_t bus, uint32_t addr, const uint8_t *tx, size_t tx_len,
                    uint8_t *rx, size_t rx_len, uint32_t timeout_ms)
{
    uint64_t deadline;
    int      rc;

    if (in_handler || pending.active)
    {
        return FS_UMB_BUSY;
    }
    if (tx_len + 2 > HIL_MAX_PAYLOAD || rx_len > HIL_MAX_PAYLOAD)
    {
        return FS_UMB_TOO_LONG;
    }

    hil_put_u16(txn_payload, (uint16_t)rx_len);
    if (tx_len > 0)
    {
        memcpy(&txn_payload[2], tx, tx_len);
    }

    pending.active = 1;
    pending.done   = 0;
    pending.type   = (uint8_t)(req_type + 1); /* the response type follows the request type */
    pending.seq    = next_seq++;
    pending.rx     = rx;
    pending.rx_len = rx_len;

    rc = send_frame(req_type, bus, pending.seq, addr, txn_payload, tx_len + 2);
    if (rc == FS_UMB_OK)
    {
        deadline = fs_hal_time_us() + (uint64_t)timeout_ms * 1000u;
        while (!pending.done && fs_hal_time_us() < deadline)
        {
            fs_umb_poll();
            if (!pending.done)
            {
                fs_hal_sleep_us(50);
            }
        }
        if (!pending.done)
        {
            stats.timeouts++;
            rc = FS_UMB_TIMEOUT;
        }
        else
        {
            rc = pending.status;
            if (rc != HIL_STATUS_OK)
            {
                stats.bus_errors++;
            }
        }
    }
    pending.active = 0;
    return rc;
}

int fs_umb_i2c(uint8_t bus, uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
               uint32_t timeout_ms)
{
    return transact(HIL_I2C_TXN, bus, addr, tx, tx_len, rx, rx_len, timeout_ms);
}

int fs_umb_spi(uint8_t bus, uint8_t cs, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
               uint32_t timeout_ms)
{
    return transact(HIL_SPI_TXN, bus, cs, tx, tx_len, rx, rx_len, timeout_ms);
}

int fs_umb_can(uint8_t bus, uint32_t id, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
               uint32_t timeout_ms)
{
    return transact(HIL_CAN_TXN, bus, id, tx, tx_len, rx, rx_len, timeout_ms);
}

/* ---- UART ---- */

int fs_umb_uart_open(uint8_t bus)
{
    unsigned i;

    if (find_ring(bus) != NULL)
    {
        return FS_UMB_OK;
    }
    for (i = 0; i < UART_SLOTS; i++)
    {
        if (uart_rings[i].bus == NO_BUS)
        {
            uart_rings[i].bus  = bus;
            uart_rings[i].head = 0;
            uart_rings[i].tail = 0;
            return send_frame(HIL_UART_OPEN, bus, next_seq++, 0, NULL, 0);
        }
    }
    return FS_UMB_TOO_LONG; /* all slots in use */
}

int fs_umb_uart_write(uint8_t bus, const uint8_t *data, size_t len)
{
    if (find_ring(bus) == NULL)
    {
        int rc = fs_umb_uart_open(bus);
        if (rc != FS_UMB_OK)
        {
            return rc;
        }
    }
    return send_frame(HIL_UART_TX, bus, next_seq++, 0, data, len);
}

size_t fs_umb_uart_available(uint8_t bus)
{
    uart_ring_t *r = find_ring(bus);

    if (r == NULL)
    {
        return 0;
    }
    return (size_t)((r->head + UART_RING_SIZE - r->tail) % UART_RING_SIZE);
}

size_t fs_umb_uart_read(uint8_t bus, uint8_t *buf, size_t max)
{
    uart_ring_t *r = find_ring(bus);
    size_t       n = 0;

    if (r == NULL)
    {
        return 0;
    }
    while (n < max && r->tail != r->head)
    {
        buf[n++] = r->data[r->tail];
        r->tail  = (uint16_t)((r->tail + 1) % UART_RING_SIZE);
    }
    return n;
}

void fs_umb_uart_flush(uint8_t bus)
{
    uart_ring_t *r = find_ring(bus);

    if (r != NULL)
    {
        r->tail = r->head;
    }
}

/* ---- Torquers, packets, log ---- */

int fs_umb_trq(uint8_t torquer, int16_t duty)
{
    uint8_t p[2];

    hil_put_u16(p, (uint16_t)duty);
    return send_frame(HIL_TRQ_CMD, torquer, next_seq++, 0, p, sizeof(p));
}

int fs_umb_heartbeat(void)
{
    return send_frame(HIL_HEARTBEAT, 0, next_seq++, 0, NULL, 0);
}

int fs_umb_send_tm(const uint8_t *pkt, size_t len)
{
    return send_frame(HIL_TO_PKT, 0, next_seq++, 0, pkt, len);
}

int fs_umb_send_rf(const uint8_t *frame, size_t len)
{
    return send_frame(HIL_RF_TX, 0, next_seq++, 0, frame, len);
}

void fs_umb_log(const char *fmt, ...)
{
    char    text[128];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    send_frame(HIL_LOG, 0, next_seq++, 0, text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
}

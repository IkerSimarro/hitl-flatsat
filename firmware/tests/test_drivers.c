/*
** Unit tests of the drivers for the FlatSat's own boards, against register-level models of their chips
** (procedure step TC-02.4). Runs on Linux with the fake HAL.
*/
#include <stdio.h>
#include <string.h>

#include "fake_hal.h"
#include "fake_ina219.h"
#include "fake_mcp2515.h"
#include "fake_sx1262.h"
#include "ina219.h"
#include "mcp2515.h"
#include "quadrature.h"
#include "sx1262.h"

static int checks, failures;

#define CHECK(cond)                                                                                            \
    do                                                                                                         \
    {                                                                                                          \
        checks++;                                                                                              \
        if (!(cond))                                                                                           \
        {                                                                                                      \
            failures++;                                                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
        }                                                                                                      \
    } while (0)

/* ---- MCP2515 CAN controller ---- */

static mcp2515_t can_ctl = {.xfer = fake_mcp2515_xfer};

static int frames_equal(const fs_can_frame_t *a, const fs_can_frame_t *b)
{
    return a->id == b->id && a->dlc == b->dlc && memcmp(a->data, b->data, a->dlc) == 0;
}

static void test_mcp2515(void)
{
    static const fs_can_frame_t frames[] = {
        {0x123, 3, {0xAA, 0x55, 0x01}},
        {0x7FF, 8, {1, 2, 3, 4, 5, 6, 7, 8}},
        {0x000, 0, {0}},
        {0x0A5, 1, {0x00}},
    };
    fs_can_frame_t f, bad;
    uint8_t        tec, rec;
    unsigned       i;
    int            got;

    /* Not fitted: a floating bus reads 0xFF */
    fake_mcp2515_present = 0;
    CHECK(mcp2515_init(&can_ctl) == MCP2515_ERR_CHIP);
    fake_mcp2515_present = 1;

    /* Initialisation: 500 kbit/s timing, every frame accepted with rollover, polled, normal mode */
    fake_mcp2515_reset();
    fake_mcp2515_regs[MCP2515_REG_CANINTE] = 0xFF;
    CHECK(mcp2515_init(&can_ctl) == MCP2515_OK);
    CHECK(fake_mcp2515_regs[MCP2515_REG_CNF1] == 0x00 && fake_mcp2515_regs[MCP2515_REG_CNF2] == 0xBC &&
          fake_mcp2515_regs[MCP2515_REG_CNF3] == 0x01);
    CHECK((fake_mcp2515_regs[MCP2515_REG_CANSTAT] & MCP2515_MODE_MASK) == MCP2515_MODE_NORMAL);
    CHECK(fake_mcp2515_regs[MCP2515_REG_RXB0CTRL] == 0x64 && fake_mcp2515_regs[MCP2515_REG_RXB1CTRL] == 0x60);
    CHECK(fake_mcp2515_regs[MCP2515_REG_CANINTE] == 0x00);

    /* The bit timing: 16 quanta of 2 / 16 MHz = 2 us = 500 kbit/s, sampled after 14 of them (87.5 %) */
    {
        unsigned brp = (fake_mcp2515_regs[MCP2515_REG_CNF1] & 0x3F) + 1u;
        unsigned prop = (fake_mcp2515_regs[MCP2515_REG_CNF2] & 0x07) + 1u;
        unsigned ps1 = ((fake_mcp2515_regs[MCP2515_REG_CNF2] >> 3) & 0x07) + 1u;
        unsigned ps2 = (fake_mcp2515_regs[MCP2515_REG_CNF3] & 0x07) + 1u;
        unsigned quanta = 1u + prop + ps1 + ps2;
        CHECK(16000000u / (2u * brp * quanta) == 500000u);
        CHECK((1u + prop + ps1) * 1000u / quanta == 875u);
    }

    /* Transmission: identifier, length and data on the bus as given; invalid frames refused */
    fake_mcp2515_sent_count = 0;
    for (i = 0; i < sizeof(frames) / sizeof(frames[0]); i++)
    {
        CHECK(mcp2515_send(&can_ctl, &frames[i]) == MCP2515_OK);
    }
    CHECK(fake_mcp2515_sent_count == 4);
    for (i = 0; i < 4 && i < fake_mcp2515_sent_count; i++)
    {
        CHECK(frames_equal(&fake_mcp2515_sent[i], &frames[i]));
    }
    bad     = frames[0];
    bad.id  = 0x800;
    CHECK(mcp2515_send(&can_ctl, &bad) == MCP2515_ERR_FRAME);
    bad     = frames[0];
    bad.dlc = 9;
    CHECK(mcp2515_send(&can_ctl, &bad) == MCP2515_ERR_FRAME);

    /* Three transmit buffers: a fourth frame waits until one is free */
    fake_mcp2515_tx_hold    = 1;
    fake_mcp2515_sent_count = 0;
    CHECK(mcp2515_send(&can_ctl, &frames[0]) == MCP2515_OK);
    CHECK(mcp2515_send(&can_ctl, &frames[1]) == MCP2515_OK);
    CHECK(mcp2515_send(&can_ctl, &frames[2]) == MCP2515_OK);
    CHECK(mcp2515_send(&can_ctl, &frames[3]) == MCP2515_ERR_BUSY);
    fake_mcp2515_complete_tx();
    fake_mcp2515_tx_hold = 0;
    CHECK(fake_mcp2515_sent_count == 3);
    CHECK(mcp2515_send(&can_ctl, &frames[3]) == MCP2515_OK && fake_mcp2515_sent_count == 4);

    /* Reception: in order of arrival through both buffers; extended and remote frames dropped */
    CHECK(mcp2515_recv(&can_ctl, &f) == 0);
    fake_mcp2515_receive(0x123, 0, 0, 3, frames[0].data);
    fake_mcp2515_receive(0x7FF, 0, 0, 8, frames[1].data);
    CHECK(mcp2515_recv(&can_ctl, &f) == 1 && frames_equal(&f, &frames[0]));
    CHECK(mcp2515_recv(&can_ctl, &f) == 1 && frames_equal(&f, &frames[1]));
    CHECK(mcp2515_recv(&can_ctl, &f) == 0);
    fake_mcp2515_receive(0x1ABCDEF, 1, 0, 2, frames[0].data);
    fake_mcp2515_receive(0x123, 0, 1, 0, frames[0].data);
    CHECK(mcp2515_recv(&can_ctl, &f) == 0);
    fake_mcp2515_receive(0x0A5, 0, 0, 1, frames[3].data);
    got = mcp2515_recv(&can_ctl, &f);
    CHECK(got == 1 && frames_equal(&f, &frames[3]));

    /* Three frames with no reading in between: the third is lost, and counted */
    fake_mcp2515_receive(0x101, 0, 0, 0, NULL);
    fake_mcp2515_receive(0x102, 0, 0, 0, NULL);
    fake_mcp2515_receive(0x103, 0, 0, 0, NULL);
    CHECK(mcp2515_service(&can_ctl) == MCP2515_OK && can_ctl.rx_overflows == 1);
    CHECK((fake_mcp2515_regs[MCP2515_REG_EFLG] & 0xC0) == 0);
    CHECK(mcp2515_recv(&can_ctl, &f) == 1 && f.id == 0x101);
    CHECK(mcp2515_recv(&can_ctl, &f) == 1 && f.id == 0x102);
    CHECK(mcp2515_recv(&can_ctl, &f) == 0);

    /* Error counters, and bus-off counted once per event */
    fake_mcp2515_regs[MCP2515_REG_TEC] = 0x12;
    fake_mcp2515_regs[MCP2515_REG_REC] = 0x34;
    CHECK(mcp2515_error_counters(&can_ctl, &tec, &rec) == MCP2515_OK && tec == 0x12 && rec == 0x34);
    fake_mcp2515_regs[MCP2515_REG_EFLG] = 0x20;
    mcp2515_service(&can_ctl);
    mcp2515_service(&can_ctl);
    fake_mcp2515_regs[MCP2515_REG_EFLG] = 0x00;
    mcp2515_service(&can_ctl);
    fake_mcp2515_regs[MCP2515_REG_EFLG] = 0x20;
    mcp2515_service(&can_ctl);
    CHECK(can_ctl.bus_off == 2);
}

/* ---- INA219 power monitors ---- */

static void test_ina219(void)
{
    ina219_t mon[3];
    uint16_t mv;
    int16_t  ma;
    unsigned i;

    fake_ina219_reset();
    for (i = 0; i < 3; i++)
    {
        mon[i].write = fake_ina219_write;
        mon[i].read  = fake_ina219_read;
        mon[i].ctx   = NULL;
        mon[i].addr  = fake_ina219[i].addr;
        CHECK(ina219_init(&mon[i]) == INA219_OK);
        CHECK(fake_ina219[i].regs[INA219_REG_CONFIG] == 0x0DDF && fake_ina219[i].regs[INA219_REG_CAL] == 4096);
    }

    /* Readings: 4 mV and 0.1 mA steps, current rounded to the nearest mA, either direction */
    fake_ina219[0].true_mv = 3940.0;
    fake_ina219[0].true_ma = 142.0;
    CHECK(ina219_read(&mon[0], &mv, &ma) == INA219_OK && mv == 3940 && ma == 142);
    fake_ina219[1].true_mv = 5001.0; /* 4 mV resolution */
    fake_ina219[1].true_ma = -37.4;
    CHECK(ina219_read(&mon[1], &mv, &ma) == INA219_OK && mv == 5000 && ma == -37);
    fake_ina219[2].true_ma = 77.5;
    CHECK(ina219_read(&mon[2], &mv, &ma) == INA219_OK && ma == 78);
    fake_ina219[2].true_ma = 0.0;
    CHECK(ina219_read(&mon[2], &mv, &ma) == INA219_OK && ma == 0);

    /* Beyond 800 mA the measurement overflows: reported, not a wrong number */
    fake_ina219[0].true_ma = 900.0;
    CHECK(ina219_read(&mon[0], &mv, &ma) == INA219_ERR_RANGE);

    /* Not fitted (no acknowledge), and another chip at the address (configuration doesn't read back) */
    fake_ina219[1].present = 0;
    CHECK(ina219_init(&mon[1]) == INA219_ERR_I2C && ina219_read(&mon[1], &mv, &ma) == INA219_ERR_I2C);
    fake_ina219[2].ignore_writes = 1;
    fake_ina219[2].regs[INA219_REG_CONFIG] = 0x399F;
    CHECK(ina219_init(&mon[2]) == INA219_ERR_CHIP);
}

/* ---- Quadrature decoding of the wheel encoder ---- */

static void test_quadrature(void)
{
    static const int fwd[4][2] = {{1, 0}, {1, 1}, {0, 1}, {0, 0}}; /* from 00, A leads */
    quad_t           q;
    int              i;

    quad_init(&q, 0, 0);
    for (i = 0; i < 4 * 210; i++) /* one output-shaft revolution: 210 pulses */
    {
        quad_update(&q, fwd[i % 4][0], fwd[i % 4][1]);
    }
    CHECK(q.count == 840 && q.errors == 0);
    for (i = 4 * 210 - 1; i >= 0; i--) /* and back */
    {
        quad_update(&q, fwd[(i + 3) % 4][0], fwd[(i + 3) % 4][1]);
    }
    CHECK(q.count == 0 && q.errors == 0);

    /* Repeated levels (bounce on one channel back to where it was) cancel out */
    quad_update(&q, 1, 0);
    quad_update(&q, 0, 0);
    quad_update(&q, 1, 0);
    CHECK(q.count == 1 && q.errors == 0);

    /* Both channels changing at once: a missed edge, counted as an error, not a step */
    quad_update(&q, 0, 1);
    CHECK(q.count == 1 && q.errors == 1);

    /* The count wraps rather than overflowing */
    q.count = INT32_MAX;
    q.state = 0;
    quad_update(&q, 1, 0);
    CHECK(q.count == INT32_MIN);
}

/* ---- SX1262 radio ---- */

static void test_sx1262(void)
{
    static const uint8_t beacon[10] = {0x46, 0x53, 1, 2, 3, 0, 0, 0xFF, 0x10, 0x00};
    sx1262_t             radio = {.xfer = fake_sx1262_xfer, .busy = fake_sx1262_busy,
                                  .reset = fake_sx1262_reset_line, .freq_hz = 869525000u, .power_dbm = 2};
    uint8_t              buf[256];
    size_t               len = 0;
    int16_t              rssi = 0;
    int8_t               snr = 0;
    uint64_t             f;

    /* Not fitted: BUSY floats high */
    fake_sx1262.present = 0;
    fake_sx1262_power_on();
    CHECK(sx1262_init(&radio) == SX1262_ERR_BUSY);

    /* Configuration for ICD 7.1 and the board (TCXO 1.7 V from DIO3, RF switch on DIO2), then listening */
    fake_sx1262.present = 1;
    fake_sx1262_power_on();
    CHECK(sx1262_init(&radio) == SX1262_OK);
    CHECK(fake_sx1262.tcxo[0] == 0x01 && fake_sx1262.tcxo[1] == 0 && fake_sx1262.tcxo[2] == 0x01 &&
          fake_sx1262.tcxo[3] == 0x40); /* 320 x 15.625 us = 5 ms */
    CHECK(fake_sx1262.calibrated && fake_sx1262.image_calibrated && fake_sx1262.rf_switch == 1);
    CHECK(fake_sx1262.packet_type == 0x01);
    f = (uint64_t)fake_sx1262.freq_reg * 32000000u >> 25; /* back to Hz: within one step (0.95 Hz) */
    CHECK(f >= 869524999u && f <= 869525000u);
    CHECK(fake_sx1262.modulation[0] == 7 && fake_sx1262.modulation[1] == 0x04 && fake_sx1262.modulation[2] == 1 &&
          fake_sx1262.modulation[3] == 0);
    CHECK(fake_sx1262.packet[0] == 0 && fake_sx1262.packet[1] == 8 && fake_sx1262.packet[2] == 0 &&
          fake_sx1262.packet[4] == 1 && fake_sx1262.packet[5] == 0);
    CHECK(fake_sx1262.regs[0x0740] == 0x14 && fake_sx1262.regs[0x0741] == 0x24);
    CHECK(fake_sx1262.pa[0] == 4 && fake_sx1262.pa[1] == 7 && fake_sx1262.pa[2] == 0 && fake_sx1262.pa[3] == 1);
    CHECK(fake_sx1262.tx_params[0] == 2 && fake_sx1262.tx_params[1] == 0x04);
    CHECK((fake_sx1262.regs[SX1262_REG_TX_CLAMP] & 0x1E) == 0x1E);
    CHECK(fake_sx1262.dio1_mask == fake_sx1262.irq_mask && (fake_sx1262.irq_mask & SX1262_IRQ_RX_DONE));
    CHECK(fake_sx1262.mode == FAKE_SX1262_RX && fake_sx1262.rx_timeout == 0xFFFFFF);
    CHECK(sx1262_poll(&radio, buf, sizeof(buf), &len, &rssi, &snr) == SX1262_EVENT_NONE);

    /* A transmission: the frame on the air with its length, then listening again */
    CHECK(sx1262_send(&radio, beacon, sizeof(beacon)) == SX1262_OK);
    CHECK(fake_sx1262.sent_count == 1 && fake_sx1262.sent_len == sizeof(beacon) &&
          memcmp(fake_sx1262.sent, beacon, sizeof(beacon)) == 0);
    CHECK(sx1262_poll(&radio, buf, sizeof(buf), &len, &rssi, &snr) == SX1262_EVENT_TX_DONE);
    CHECK(fake_sx1262.mode == FAKE_SX1262_RX && fake_sx1262.packet[3] == 0xFF && !radio.transmitting);

    /* One transmission at a time; frames that can't be sent */
    fake_sx1262.tx_hold = 1;
    CHECK(sx1262_send(&radio, beacon, sizeof(beacon)) == SX1262_OK);
    CHECK(sx1262_send(&radio, beacon, sizeof(beacon)) == SX1262_ERR_TX);
    CHECK(sx1262_poll(&radio, buf, sizeof(buf), &len, &rssi, &snr) == SX1262_EVENT_NONE && radio.transmitting);
    fake_sx1262_tx_end();
    fake_sx1262.tx_hold = 0;
    CHECK(sx1262_poll(&radio, buf, sizeof(buf), &len, &rssi, &snr) == SX1262_EVENT_TX_DONE);
    CHECK(sx1262_send(&radio, beacon, 0) == SX1262_ERR_ARG && sx1262_send(&radio, buf, 256) == SX1262_ERR_ARG);

    /* Reception: the frame with its RSSI and SNR; corrupted frames counted and dropped */
    fake_sx1262_air(beacon, sizeof(beacon), -97, -12, 0);
    CHECK(sx1262_poll(&radio, buf, sizeof(buf), &len, &rssi, &snr) == SX1262_EVENT_RX);
    CHECK(len == sizeof(beacon) && memcmp(buf, beacon, len) == 0 && rssi == -97 && snr == -12);
    CHECK(sx1262_poll(&radio, buf, sizeof(buf), &len, &rssi, &snr) == SX1262_EVENT_NONE);
    fake_sx1262_air(beacon, sizeof(beacon), -110, 4, 1);
    CHECK(sx1262_poll(&radio, buf, sizeof(buf), &len, &rssi, &snr) == SX1262_EVENT_NONE && radio.crc_errors == 1);

    /* Transmit power changes in range; out of range refused */
    CHECK(sx1262_set_power(&radio, 10) == SX1262_OK && fake_sx1262.tx_params[0] == 10 && radio.power_dbm == 10);
    CHECK(sx1262_set_power(&radio, -9) == SX1262_OK && (int8_t)fake_sx1262.tx_params[0] == -9);
    CHECK(sx1262_set_power(&radio, 23) == SX1262_ERR_ARG && sx1262_set_power(&radio, -10) == SX1262_ERR_ARG);

    /* A frame longer than the caller's buffer is cut to fit */
    fake_sx1262_air(beacon, sizeof(beacon), -90, 20, 0);
    CHECK(sx1262_poll(&radio, buf, 4, &len, &rssi, &snr) == SX1262_EVENT_RX && len == 4);
}

int main(void)
{
    fake_hal_reset();
    test_mcp2515();
    test_ina219();
    test_quadrature();
    test_sx1262();
    printf("board drivers: %s%d checks%s\n", failures ? "" : "all ", checks, failures ? "" : " passed");
    if (failures)
    {
        printf("%d of %d checks failed\n", failures, checks);
    }
    return failures ? 1 : 0;
}

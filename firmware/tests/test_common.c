/*
** Unit tests for the firmware common libraries, run against the fake HAL
*/
#include <stdio.h>
#include <string.h>

#include "fake_hal.h"
#include "flatsat_icd.h"
#include "fs_ccsds.h"
#include "fs_hal.h"
#include "fs_persist.h"
#include "fs_sched.h"
#include "fs_time.h"
#include "fs_umbilical.h"
#include "hil_link.h"

static int failures;
static int checks;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        checks++;                                                  \
        if (!(cond))                                               \
        {                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

/* ---- CCSDS ---- */

static void test_cmd_matches_python_codec(void)
{
    /* build_command("OBC_SET_MODE", MODE=1) from ground/flatsat_icd.py, also checked against COSMOS */
    static const uint8_t expected[] = {0x1a, 0x00, 0xc0, 0x00, 0x00, 0x02, 0x02, 0x24, 0x01};
    flatsat_obc_set_mode_t args = {.mode = FLATSAT_MODE_DETUMBLE};
    uint8_t buf[32];
    size_t  n = fs_cmd_build(buf, sizeof(buf), FLATSAT_OBC_CMD_MID, FLATSAT_OBC_SET_MODE_FC, &args, sizeof(args));

    CHECK(n == FLATSAT_OBC_SET_MODE_LEN);
    CHECK(n == sizeof(expected) && memcmp(buf, expected, n) == 0);
}

static void test_cmd_parse(void)
{
    flatsat_obc_ping_t args = {.token = 0xDEADBEEF};
    uint8_t  buf[32];
    fs_cmd_t cmd;
    size_t   n = fs_cmd_build(buf, sizeof(buf), FLATSAT_OBC_CMD_MID, FLATSAT_OBC_PING_FC, &args, sizeof(args));

    CHECK(fs_cmd_parse(buf, n, &cmd) == FS_CMD_OK);
    CHECK(cmd.mid == FLATSAT_OBC_CMD_MID && cmd.fc == FLATSAT_OBC_PING_FC);
    CHECK(cmd.args_len == sizeof(args) && memcmp(cmd.args, &args, sizeof(args)) == 0);

    CHECK(fs_cmd_parse(buf, 5, &cmd) == FS_CMD_TOO_SHORT);
    CHECK(fs_cmd_parse(buf, n - 1, &cmd) == FS_CMD_LENGTH_MISMATCH);
    buf[n - 1] ^= 0x01;
    CHECK(fs_cmd_parse(buf, n, &cmd) == FS_CMD_BAD_CHECKSUM);
    buf[n - 1] ^= 0x01;
    buf[0] = 0x0A; /* telemetry MID: not a command */
    CHECK(fs_cmd_parse(buf, n, &cmd) == FS_CMD_NOT_COMMAND);
}

static void test_tlm_header_and_sequences(void)
{
    flatsat_beacon_t beacon;
    uint8_t          buf[128];
    size_t           n;
    int              i;

    fs_tlm_reset_sequences();
    memset(&beacon, 0, sizeof(beacon));
    beacon.uptime = 42;

    n = fs_tlm_build(buf, sizeof(buf), FLATSAT_BEACON_MID, 814254200u, 0x8000, &beacon, sizeof(beacon));
    CHECK(n == FLATSAT_BEACON_LEN);
    CHECK(buf[0] == 0x0A && buf[1] == 0x01);
    CHECK(buf[2] == 0xC0 && buf[3] == 0x00);                          /* unsegmented, count 0 */
    CHECK(((buf[4] << 8) | buf[5]) == (int)n - 7);                   /* length field */
    CHECK(buf[6] == 0x30 && buf[7] == 0x88 && buf[8] == 0x88 && buf[9] == 0x78); /* 814254200 = 0x30888878, BE */
    CHECK(buf[10] == 0x80 && buf[11] == 0x00);
    CHECK(buf[16] == 42);                                             /* payload is little-endian */

    /* Each MID has its own counter */
    n = fs_tlm_build(buf, sizeof(buf), FLATSAT_OBC_HK_MID, 0, 0, NULL, 0);
    CHECK(buf[3] == 0);
    n = fs_tlm_build(buf, sizeof(buf), FLATSAT_BEACON_MID, 0, 0, &beacon, sizeof(beacon));
    CHECK(buf[3] == 1);

    /* The 14-bit count wraps to 0 */
    for (i = 2; i <= 0x3FFF; i++)
    {
        fs_tlm_build(buf, sizeof(buf), FLATSAT_BEACON_MID, 0, 0, &beacon, sizeof(beacon));
    }
    fs_tlm_build(buf, sizeof(buf), FLATSAT_BEACON_MID, 0, 0, &beacon, sizeof(beacon));
    CHECK(buf[2] == 0xC0 && buf[3] == 0x00);

    CHECK(fs_tlm_build(buf, 10, FLATSAT_BEACON_MID, 0, 0, &beacon, sizeof(beacon)) == 0);
}

/* ---- Time ---- */

static void test_time(void)
{
    fs_time_t start = {814254200u, 0};
    fs_time_t t;
    fs_time_t ref;
    uint64_t  us;

    fake_hal_reset();

    /* Conversion round trip is exact to within one subsecond step (2^-16 s, about 15.3 us) */
    for (us = 0; us < 3000000u; us += 123457u)
    {
        uint64_t back = fs_time_to_us(fs_time_from_us(814254200000000ull + us));
        uint64_t diff = back > 814254200000000ull + us ? back - (814254200000000ull + us)
                                                       : 814254200000000ull + us - back;
        CHECK(diff <= 16);
    }

    fs_time_init(start);
    fake_now_us += 1500000u;
    t = fs_time_now();
    CHECK(t.seconds == 814254201u && t.subseconds == 0x8000);
    CHECK(fs_time_source() == FLATSAT_TIME_SOURCE_FREE_RUNNING);

    /* Reference 0.25 s ahead: clock steps forward and records +250 ms */
    ref.seconds    = 814254201u;
    ref.subseconds = 0xC000;
    fs_time_sync(ref, 1);
    CHECK(fs_time_last_correction_us() > 249980 && fs_time_last_correction_us() < 250020);
    t = fs_time_now();
    CHECK(t.seconds == 814254201u && t.subseconds == 0xC000);
    CHECK(fs_time_source() == FLATSAT_TIME_SOURCE_UMBILICAL);

    /* No more references: free-running after the timeout */
    fake_now_us += FS_TIME_SYNC_TIMEOUT_US + 1;
    CHECK(fs_time_source() == FLATSAT_TIME_SOURCE_FREE_RUNNING);
}

/* ---- Scheduler ---- */

static unsigned fast_runs;
static unsigned slow_runs;
static uint32_t slow_cost_us;

static void fast_task(void)
{
    fast_runs++;
    fake_now_us += 100; /* 100 us of work */
}

static void slow_task(void)
{
    slow_runs++;
    fake_now_us += slow_cost_us;
}

static void test_sched(void)
{
    fs_task_t tasks[] = {
        {.name = "fast", .period_ms = 10, .offset_ms = 0, .fn = fast_task},
        {.name = "slow", .period_ms = 100, .offset_ms = 5, .fn = slow_task},
    };
    uint64_t end;

    fake_hal_reset();
    fast_runs = slow_runs = 0;
    slow_cost_us          = 1000;
    fs_sched_init(tasks, 2);

    /* Just over one simulated second (load is reported per completed 1 s window), stepping like a
     * main loop that sleeps until the next task */
    end = fake_now_us + 1005000u;
    while (fake_now_us < end)
    {
        uint32_t wait = fs_sched_run();
        fake_now_us += wait > 0 ? wait : 1;
    }
    CHECK(fast_runs >= 99 && fast_runs <= 101);
    CHECK(slow_runs == 10);
    CHECK(tasks[0].overruns == 0 && tasks[1].overruns == 0);
    CHECK(tasks[1].max_exec_us == 1000);
    /* Busy 100 x 100 us + 10 x 1000 us = 20 ms per second = 2 % */
    CHECK(fs_sched_cpu_load() >= 1 && fs_sched_cpu_load() <= 3);

    /* A task that takes longer than its period is counted as an overrun, not run back-to-back */
    slow_cost_us = 250000;
    end          = fake_now_us + 1000000u;
    slow_runs    = 0;
    while (fake_now_us < end)
    {
        uint32_t wait = fs_sched_run();
        fake_now_us += wait > 0 ? wait : 1;
    }
    CHECK(tasks[1].overruns > 0);
    CHECK(slow_runs <= 5);
}

/* ---- Umbilical ---- */

static fs_time_t last_time;
static int       time_frames;
static uint8_t   last_cmd[64];
static size_t    last_cmd_len;
static int       nested_result;

static void on_time(fs_time_t t)
{
    last_time = t;
    time_frames++;
}

static void on_command(const uint8_t *pkt, size_t len)
{
    uint8_t rx[4];
    memcpy(last_cmd, pkt, len);
    last_cmd_len = len;
    /* Handlers must not start transactions */
    nested_result = fs_umb_i2c(1, 0x2B, pkt, 1, rx, sizeof(rx), 10);
}

static void test_umbilical(void)
{
    fs_umb_handlers_t h = {.on_command = on_command, .on_time = on_time};
    uint8_t           tx[3] = {0x70, 0x00, 0x24};
    uint8_t           rx[65];
    uint8_t           buf[16];
    int               rc;
    int               i;

    fake_hal_reset();
    fs_umb_init(&h);

    /* I2C transaction answered by the fake bridge */
    rc = fs_umb_i2c(1, 0x2B, tx, sizeof(tx), rx, sizeof(rx), 100);
    CHECK(rc == FS_UMB_OK);
    CHECK(fake_last_type == HIL_I2C_TXN && fake_last_bus == 1 && fake_last_addr == 0x2B);
    CHECK(fake_last_len == 2 + sizeof(tx) && hil_get_u16(fake_last_payload) == sizeof(rx));
    for (i = 0; i < (int)sizeof(rx); i++)
    {
        CHECK(rx[i] == (uint8_t)(0x70 + i));
    }

    /* Bus error reported by the bridge */
    fake_bridge_status = HIL_STATUS_BUS_ERROR;
    CHECK(fs_umb_spi(2, 0, tx, 1, rx, 4, 100) == HIL_STATUS_BUS_ERROR);
    CHECK(fs_umb_stats()->bus_errors == 1);
    fake_bridge_status = HIL_STATUS_OK;

    /* No answer: timeout after the requested time, measured on the simulated clock */
    fake_bridge_silent = 1;
    {
        uint64_t start = fake_now_us;
        CHECK(fs_umb_can(0, 0x123, tx, 1, rx, 4, 20) == FS_UMB_TIMEOUT);
        CHECK(fake_now_us - start >= 20000 && fake_now_us - start < 21000);
    }
    CHECK(fs_umb_stats()->timeouts == 1);
    fake_bridge_silent = 0;

    /* TIME frame is decoded and delivered */
    {
        uint8_t p[6] = {0x78, 0x88, 0x88, 0x30, 0x00, 0x40}; /* 814254200 s (0x30888878 LE) + 0x4000 */
        fake_bridge_inject(HIL_TIME, 0, p, sizeof(p));
        fs_umb_poll();
        CHECK(time_frames == 1);
        CHECK(last_time.seconds == 814254200u && last_time.subseconds == 0x4000);
        CHECK(fs_umb_link_up());
    }

    /* Command delivered; a transaction from inside the handler is refused */
    {
        uint8_t cmd[] = {0x1a, 0x00, 0xc0, 0x00, 0x00, 0x02, 0x02, 0x24, 0x01};
        fake_bridge_inject(HIL_CI_PKT, 0, cmd, sizeof(cmd));
        fs_umb_poll();
        CHECK(last_cmd_len == sizeof(cmd) && memcmp(last_cmd, cmd, sizeof(cmd)) == 0);
        CHECK(nested_result == FS_UMB_BUSY);
    }

    /* UART bytes are buffered only for opened ports */
    {
        uint8_t data[] = {1, 2, 3, 4, 5};
        CHECK(fs_umb_uart_open(16) == FS_UMB_OK);
        CHECK(fake_last_type == HIL_UART_OPEN && fake_last_bus == 16);
        fake_bridge_inject(HIL_UART_RX, 16, data, sizeof(data));
        fake_bridge_inject(HIL_UART_RX, 3, data, sizeof(data)); /* not opened: dropped */
        fs_umb_poll();
        CHECK(fs_umb_uart_available(16) == 5);
        CHECK(fs_umb_uart_available(3) == 0);
        CHECK(fs_umb_uart_read(16, buf, 3) == 3 && buf[0] == 1 && buf[2] == 3);
        CHECK(fs_umb_uart_read(16, buf, sizeof(buf)) == 2 && buf[0] == 4);
    }

    /* Torquer command encoding: torquer in bus, duty i16 LE */
    CHECK(fs_umb_trq(2, -2500) == FS_UMB_OK);
    CHECK(fake_last_type == HIL_TRQ_CMD && fake_last_bus == 2);
    CHECK(fake_last_len == 2 && (int16_t)hil_get_u16(fake_last_payload) == -2500);

    /* Link goes down when nothing arrives */
    fake_now_us += FS_UMB_LINK_TIMEOUT_US + 1;
    CHECK(!fs_umb_link_up());
}

static void test_uart_reopen_on_link_up(void)
{
    uint8_t  p[6] = {0};
    unsigned frames;

    fake_hal_reset();
    fs_umb_init(NULL);

    /* Buses declared before the bridge is listening: the requests may be lost */
    CHECK(fs_umb_uart_open(1) == FS_UMB_OK);
    CHECK(fs_umb_i2c_open(2) == FS_UMB_OK);
    CHECK(fs_umb_spi_open(0, 2) == FS_UMB_OK);
    CHECK(fs_umb_can_open(0) == FS_UMB_OK);
    CHECK(fs_umb_can_open(0) == FS_UMB_OK); /* declaring twice sends nothing more */
    CHECK(!fs_umb_link_up());
    CHECK(fs_umb_link_up_for_us() == 0);
    frames = fake_frames_from_fw;

    /* First frame from the bridge: the client must open all four again; the last one sent is CAN */
    fake_bridge_inject(HIL_TIME, 0, p, sizeof(p));
    fs_umb_poll();
    CHECK(fs_umb_link_up());
    CHECK(fake_frames_from_fw == frames + 4);
    CHECK(fake_last_type == HIL_CAN_OPEN && fake_last_bus == 0);

    /* Further frames while the link stays up don't repeat it; the up time grows */
    fake_now_us += 500000;
    fake_bridge_inject(HIL_TIME, 0, p, sizeof(p));
    fs_umb_poll();
    CHECK(fake_frames_from_fw == frames + 4);
    CHECK(fs_umb_link_up_for_us() >= 500000 && fs_umb_link_up_for_us() < 600000);

    /* After a link loss, the next frame triggers it again and the up time restarts */
    fake_now_us += FS_UMB_LINK_TIMEOUT_US + 1;
    CHECK(fs_umb_link_up_for_us() == 0);
    fake_bridge_inject(HIL_TIME, 0, p, sizeof(p));
    fs_umb_poll();
    CHECK(fake_frames_from_fw == frames + 8 && fake_last_type == HIL_CAN_OPEN);
    CHECK(fs_umb_link_up_for_us() < 1000);
}

static void test_persistence(void)
{
    fs_persist_t p;
    int          i;

    fs_persist_init(&p, 3);

    /* Two isolated misses: counted, never a fault */
    CHECK(fs_persist_update(&p, 0) == FS_PERSIST_NO_CHANGE);
    CHECK(fs_persist_update(&p, 0) == FS_PERSIST_NO_CHANGE);
    CHECK(fs_persist_update(&p, 1) == FS_PERSIST_NO_CHANGE);
    CHECK(fs_persist_update(&p, 0) == FS_PERSIST_NO_CHANGE);
    CHECK(fs_persist_update(&p, 1) == FS_PERSIST_NO_CHANGE);
    CHECK(!p.faulted && p.misses == 3);

    /* Three in a row: declared exactly once, however long it lasts */
    CHECK(fs_persist_update(&p, 0) == FS_PERSIST_NO_CHANGE);
    CHECK(fs_persist_update(&p, 0) == FS_PERSIST_NO_CHANGE);
    CHECK(fs_persist_update(&p, 0) == FS_PERSIST_TRIPPED);
    for (i = 0; i < 300; i++)
    {
        CHECK(fs_persist_update(&p, 0) == FS_PERSIST_NO_CHANGE);
    }
    CHECK(p.faulted && p.misses == 306);

    /* First good check clears it, once */
    CHECK(fs_persist_update(&p, 1) == FS_PERSIST_CLEARED);
    CHECK(fs_persist_update(&p, 1) == FS_PERSIST_NO_CHANGE);
    CHECK(!p.faulted);

    /* Threshold 1 behaves like no filter; 0 is treated as 1 */
    fs_persist_init(&p, 0);
    CHECK(fs_persist_update(&p, 0) == FS_PERSIST_TRIPPED);
    CHECK(fs_persist_update(&p, 1) == FS_PERSIST_CLEARED);
}

int main(void)
{
    fs_hal_init(0, NULL);

    test_cmd_matches_python_codec();
    test_cmd_parse();
    test_tlm_header_and_sequences();
    test_time();
    test_sched();
    test_umbilical();
    test_uart_reopen_on_link_up();
    test_persistence();

    if (failures)
    {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("firmware common: all %d checks passed\n", checks);
    return 0;
}

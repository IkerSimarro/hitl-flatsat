/*
** Mission clock (see fs_time.h)
*/
#include "fs_time.h"

#include "flatsat_icd.h"
#include "fs_hal.h"

/* Mission time in microseconds = monotonic time + offset */
static int64_t  offset_us;
static uint64_t last_sync_us;
static int      synced_from_umbilical;
static int64_t  last_correction_us;

uint64_t fs_time_to_us(fs_time_t t)
{
    return (uint64_t)t.seconds * 1000000u + (((uint64_t)t.subseconds * 1000000u) >> 16);
}

fs_time_t fs_time_from_us(uint64_t us)
{
    fs_time_t t;
    uint64_t  frac_us = us % 1000000u;

    t.seconds    = (uint32_t)(us / 1000000u);
    t.subseconds = (uint16_t)((frac_us << 16) / 1000000u);
    return t;
}

void fs_time_init(fs_time_t start)
{
    offset_us             = (int64_t)fs_time_to_us(start) - (int64_t)fs_hal_time_us();
    last_sync_us          = 0;
    synced_from_umbilical = 0;
    last_correction_us    = 0;
}

fs_time_t fs_time_now(void)
{
    return fs_time_from_us((uint64_t)((int64_t)fs_hal_time_us() + offset_us));
}

void fs_time_sync(fs_time_t reference, int from_umbilical)
{
    uint64_t now_us   = fs_hal_time_us();
    int64_t  ref_us   = (int64_t)fs_time_to_us(reference);
    int64_t  local_us = (int64_t)now_us + offset_us;

    last_correction_us    = ref_us - local_us;
    offset_us             = ref_us - (int64_t)now_us;
    last_sync_us          = now_us;
    synced_from_umbilical = from_umbilical;
}

uint8_t fs_time_source(void)
{
    if (synced_from_umbilical && last_sync_us != 0 && fs_hal_time_us() - last_sync_us < FS_TIME_SYNC_TIMEOUT_US)
    {
        return FLATSAT_TIME_SOURCE_UMBILICAL;
    }
    return FLATSAT_TIME_SOURCE_FREE_RUNNING;
}

int64_t fs_time_last_correction_us(void)
{
    return last_correction_us;
}

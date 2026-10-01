/*
** Mission clock: CCSDS CUC time (seconds since J2000 + 2^-16 s subseconds), ICD 1.1 and 8.2
**
** The clock free-runs on the monotonic HAL timer and is stepped to the reference whenever a TIME
** frame (NOS3 simulation time over the umbilical) or an OBC_SET_TIME command arrives. Each step is
** recorded, so the drift between OBC and simulation can be reported in telemetry and measured in tests.
*/
#ifndef FS_TIME_H
#define FS_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No reference for this long means the clock is free-running (ICD 8.2) */
#define FS_TIME_SYNC_TIMEOUT_US 5000000u

typedef struct
{
    uint32_t seconds;
    uint16_t subseconds;
} fs_time_t;

/* Start the clock at the given time (e.g. the mission start time) */
void fs_time_init(fs_time_t start);

fs_time_t fs_time_now(void);

/* Step the clock to a reference; from_umbilical selects the time source reported in OBC_HK */
void fs_time_sync(fs_time_t reference, int from_umbilical);

/* flatsat_time_source_t: UMBILICAL while TIME frames keep arriving, FREE_RUNNING otherwise */
uint8_t fs_time_source(void);

/* Size of the most recent correction in microseconds: positive if the clock was behind the reference */
int64_t fs_time_last_correction_us(void);

/* Conversions; exposed for tests */
uint64_t  fs_time_to_us(fs_time_t t);
fs_time_t fs_time_from_us(uint64_t us);

#ifdef __cplusplus
}
#endif

#endif /* FS_TIME_H */

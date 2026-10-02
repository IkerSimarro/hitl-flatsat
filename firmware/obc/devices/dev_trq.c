/*
** Magnetorquer driver: duty commands through the bridge (hil_link TRQ_CMD, ICD 8.1)
*/
#include "dev_internal.h"

int dev_trq_set(uint8_t torquer, float duty)
{
    float scaled;

    if (torquer >= DEV_NUM_TRQ || !(duty >= -1.0f && duty <= 1.0f)) /* also rejects NaN */
    {
        return DEV_ERR_ARG;
    }
    scaled = duty * 10000.0f;
    return dev_map_umb(fs_umb_trq(torquer, (int16_t)(scaled >= 0 ? scaled + 0.5f : scaled - 0.5f)));
}

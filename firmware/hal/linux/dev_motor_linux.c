/*
** ADCS wheel motor on Linux (SIL): pins and encoder through the SIL harness; the plant model does the physics
*/
#include <stddef.h>

#include "dev_motor.h"
#include "sil_harness.h"

static sil_harness_t *h;

int dev_motor_init(void)
{
    h = sil_harness();
    return h != NULL ? 0 : -1;
}

void dev_motor_set_duty(float duty)
{
    h->motor_duty = duty > 1.0f ? 1.0f : (duty < -1.0f ? -1.0f : duty);
}

int32_t dev_motor_encoder_count(void)
{
    return h->encoder_count;
}

int dev_motor_driver_enabled(void)
{
    return h->motor_nsleep != 0;
}

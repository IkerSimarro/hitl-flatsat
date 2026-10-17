/*
** ADCS node wheel motor hardware on the Pico 2 (dev_motor.h; pins in hardware/PINOUT.md)
**
**   driver    DRV8833 AIN1 (GP16) / AIN2 (GP17), PWM slice 0 at 20 kHz (above hearing, within the DRV8833's
**             range). Slow decay: the driven input stays high and the other carries the inverted duty, so the
**             motor brakes between pulses and its speed follows the duty almost linearly, as the SIL plant model
**             assumes. A duty of 0 leaves both inputs low: coasting, which is what "drive removed" means
**             (loss of commands, TC-13.4).
**   encoder   Hall channels A (GP13) / B (GP14): an interrupt on every edge of either, decoded by
**             drivers/quadrature.h. At the N20's ~600 rpm that's about 8 400 edges a second.
**   nSLEEP    sensed on GP18: the EPS node drives the DRV8833's nSLEEP; high means the driver is enabled
*/
#include "dev_motor.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "quadrature.h"

#define PIN_AIN1   16
#define PIN_AIN2   17
#define PIN_ENC_A  13
#define PIN_ENC_B  14
#define PIN_NSLEEP 18
#define PWM_HZ     20000u

static volatile quad_t encoder;
static uint32_t        pwm_top;
static unsigned        slice;

static void encoder_edge(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;
    quad_update((quad_t *)&encoder, gpio_get(PIN_ENC_A), gpio_get(PIN_ENC_B));
}

int dev_motor_init(void)
{
    gpio_set_function(PIN_AIN1, GPIO_FUNC_PWM);
    gpio_set_function(PIN_AIN2, GPIO_FUNC_PWM);
    slice   = pwm_gpio_to_slice_num(PIN_AIN1);
    pwm_top = clock_get_hz(clk_sys) / PWM_HZ - 1u;
    pwm_set_wrap(slice, (uint16_t)pwm_top);
    pwm_set_both_levels(slice, 0, 0); /* coasting */
    pwm_set_enabled(slice, true);

    gpio_init(PIN_NSLEEP);
    gpio_set_dir(PIN_NSLEEP, GPIO_IN);

    gpio_init(PIN_ENC_A);
    gpio_init(PIN_ENC_B);
    gpio_set_dir(PIN_ENC_A, GPIO_IN);
    gpio_set_dir(PIN_ENC_B, GPIO_IN);
    gpio_pull_up(PIN_ENC_A); /* for open-collector Hall outputs */
    gpio_pull_up(PIN_ENC_B);
    quad_init((quad_t *)&encoder, gpio_get(PIN_ENC_A), gpio_get(PIN_ENC_B));
    gpio_set_irq_enabled_with_callback(PIN_ENC_A, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, encoder_edge);
    gpio_set_irq_enabled(PIN_ENC_B, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    return 0;
}

void dev_motor_set_duty(float duty)
{
    uint32_t off;

    duty = duty > 1.0f ? 1.0f : (duty < -1.0f ? -1.0f : duty);
    if (duty == 0.0f)
    {
        pwm_set_both_levels(slice, 0, 0);
        return;
    }
    off = (uint32_t)((1.0f - (duty > 0 ? duty : -duty)) * (float)(pwm_top + 1u) + 0.5f);
    if (duty > 0)
    {
        pwm_set_both_levels(slice, (uint16_t)(pwm_top + 1u), (uint16_t)off); /* AIN1 high, AIN2 = 1 - duty */
    }
    else
    {
        pwm_set_both_levels(slice, (uint16_t)off, (uint16_t)(pwm_top + 1u)); /* AIN2 high, AIN1 = 1 - duty */
    }
}

int32_t dev_motor_encoder_count(void)
{
    return encoder.count; /* a single 32-bit read: atomic on the Cortex-M33 */
}

int dev_motor_driver_enabled(void)
{
    return gpio_get(PIN_NSLEEP);
}

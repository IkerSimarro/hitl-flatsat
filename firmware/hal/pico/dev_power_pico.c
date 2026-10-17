/*
** EPS node power hardware on the Pico 2 (dev_power.h; pins in hardware/PINOUT.md)
**
**   rails     three INA219 monitors on I2C0 (GP8 SDA, GP9 SCL, 400 kHz): battery output 0x40, OBC + radio 0x41,
**             ADCS node 0x44 (drivers/ina219.c). A monitor that didn't answer at start-up is retried on every read,
**             so one plugged in late is picked up. Each monitor's state is logged when it changes.
**   switch 0  the ADCS node's RUN pin (GP16): held low to keep that node in reset; released by making the pin an
**             input, so this board never drives the other board's RUN pin high
**   switch 1  DRV8833 nSLEEP (GP17): high enables the wheel motor driver; its own pull-down disables it if this
**             board is off
**   charger   TP4056 CHRG (GP13) and STDBY (GP14), open-drain, active low
** Both switches start on, as the node expects (the ADCS node running, its driver enabled).
*/
#include "dev_power.h"

#include "flatsat_icd.h"
#include "fs_hal.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "ina219.h"

#define I2C_PORT       i2c0
#define I2C_HZ         400000
#define I2C_TIMEOUT_US 2000
#define PIN_SDA        8
#define PIN_SCL        9
#define PIN_ADCS_RUN   16
#define PIN_MOTOR_EN   17
#define PIN_CHRG       13
#define PIN_STDBY      14

static const uint8_t     rail_addr[DEV_POWER_NUM_RAILS] = {0x40, 0x41, 0x44};
static const char *const rail_name[DEV_POWER_NUM_RAILS] = {"battery output", "OBC", "ADCS node"};
static ina219_t          monitor[DEV_POWER_NUM_RAILS];
static int               monitor_ok[DEV_POWER_NUM_RAILS];
static int               monitor_logged[DEV_POWER_NUM_RAILS]; /* the state last logged; -1 none yet */
static int           switch_on[DEV_POWER_NUM_SWITCHES];

static int i2c_write_cb(void *ctx, uint8_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    return i2c_write_timeout_us(I2C_PORT, addr, data, len, false, I2C_TIMEOUT_US) == (int)len ? 0 : -1;
}

static void log_monitor(unsigned rail)
{
    if (monitor_ok[rail] == monitor_logged[rail])
    {
        return;
    }
    monitor_logged[rail] = monitor_ok[rail];
    if (monitor_ok[rail])
    {
        fs_hal_log("%s rail monitor (INA219 at 0x%02x) answering", rail_name[rail], rail_addr[rail]);
    }
    else
    {
        fs_hal_log("%s rail monitor (INA219 at 0x%02x) not answering", rail_name[rail], rail_addr[rail]);
    }
}

static int i2c_read_cb(void *ctx, uint8_t addr, uint8_t *data, size_t len)
{
    (void)ctx;
    return i2c_read_timeout_us(I2C_PORT, addr, data, len, false, I2C_TIMEOUT_US) == (int)len ? 0 : -1;
}

int dev_power_init(void)
{
    unsigned i;

    i2c_init(I2C_PORT, I2C_HZ);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA); /* the breakouts have their own pull-ups too */
    gpio_pull_up(PIN_SCL);

    for (i = 0; i < DEV_POWER_NUM_RAILS; i++)
    {
        monitor[i]    = (ina219_t){.write = i2c_write_cb, .read = i2c_read_cb, .ctx = NULL, .addr = rail_addr[i]};
        monitor_ok[i]     = ina219_init(&monitor[i]) == INA219_OK;
        monitor_logged[i] = -1;
        log_monitor(i);
    }

    gpio_init(PIN_ADCS_RUN);
    gpio_init(PIN_MOTOR_EN);
    gpio_init(PIN_CHRG);
    gpio_init(PIN_STDBY);
    gpio_set_dir(PIN_CHRG, GPIO_IN);
    gpio_set_dir(PIN_STDBY, GPIO_IN);
    gpio_pull_up(PIN_CHRG);
    gpio_pull_up(PIN_STDBY);
    dev_power_set_switch(DEV_POWER_SW_ADCS_RUN, 1);
    dev_power_set_switch(DEV_POWER_SW_MOTOR, 1);
    return 0; /* missing monitors show up as read errors in EPS_REAL, they don't stop the node */
}

int dev_power_read_rail(uint8_t rail, uint16_t *mv, int16_t *ma)
{
    if (rail >= DEV_POWER_NUM_RAILS)
    {
        return -1;
    }
    if (!monitor_ok[rail])
    {
        monitor_ok[rail] = ina219_init(&monitor[rail]) == INA219_OK;
        log_monitor(rail);
        if (!monitor_ok[rail])
        {
            return -1;
        }
    }
    if (ina219_read(&monitor[rail], mv, ma) != INA219_OK)
    {
        monitor_ok[rail] = 0; /* re-initialised next time: a monitor that reset lost its calibration */
        log_monitor(rail);
        return -1;
    }
    return 0;
}

void dev_power_set_switch(uint8_t sw, int on)
{
    if (sw == DEV_POWER_SW_ADCS_RUN)
    {
        if (on)
        {
            gpio_set_dir(PIN_ADCS_RUN, GPIO_IN); /* released: the node's own pull-up lets it run */
        }
        else
        {
            gpio_put(PIN_ADCS_RUN, 0);
            gpio_set_dir(PIN_ADCS_RUN, GPIO_OUT);
        }
    }
    else if (sw == DEV_POWER_SW_MOTOR)
    {
        gpio_put(PIN_MOTOR_EN, on != 0);
        gpio_set_dir(PIN_MOTOR_EN, GPIO_OUT);
    }
    else
    {
        return;
    }
    switch_on[sw] = on != 0;
}

int dev_power_get_switch(uint8_t sw)
{
    return sw < DEV_POWER_NUM_SWITCHES ? switch_on[sw] : 0;
}

uint8_t dev_power_charge_state(void)
{
    if (!gpio_get(PIN_CHRG))
    {
        return FLATSAT_CHARGE_STATE_CHARGING;
    }
    if (!gpio_get(PIN_STDBY))
    {
        return FLATSAT_CHARGE_STATE_FULL;
    }
    return FLATSAT_CHARGE_STATE_IDLE; /* no input power, or no battery */
}

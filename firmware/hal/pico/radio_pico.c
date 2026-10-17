/*
** The SX1262 bound to the Pico's pins (see radio_pico.h)
*/
#include "radio_pico.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#define SPI_PORT  spi1
#define SPI_HZ    8000000 /* the SX1262 takes up to 16 MHz */
#define PIN_SCK   10
#define PIN_MOSI  11
#define PIN_MISO  12
#define PIN_CS    3
#define PIN_BUSY  2
#define PIN_RESET 15

static int xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    gpio_put(PIN_CS, 0);
    if (rx != NULL)
    {
        spi_write_read_blocking(SPI_PORT, tx, rx, len);
    }
    else
    {
        spi_write_blocking(SPI_PORT, tx, len);
    }
    gpio_put(PIN_CS, 1);
    return 0;
}

static int busy(void *ctx)
{
    (void)ctx;
    return gpio_get(PIN_BUSY);
}

static void reset(void *ctx, int asserted)
{
    (void)ctx;
    gpio_put(PIN_RESET, asserted ? 0 : 1);
}

void pico_sx1262_bind(sx1262_t *radio)
{
    spi_init(SPI_PORT, SPI_HZ);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    gpio_init(PIN_RESET);
    gpio_set_dir(PIN_RESET, GPIO_OUT);
    gpio_put(PIN_RESET, 1);
    gpio_init(PIN_BUSY);
    gpio_set_dir(PIN_BUSY, GPIO_IN);
    gpio_pull_up(PIN_BUSY); /* without a radio fitted, BUSY reads high: sx1262_init reports it absent */

    radio->xfer  = xfer;
    radio->busy  = busy;
    radio->reset = reset;
    radio->ctx   = NULL;
}

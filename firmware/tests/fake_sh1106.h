/*
** Command-level model of an SH1106 OLED controller on I2C (tests/test_drivers.c): its 8 pages of 132 columns of
** display memory, page and column addressing with auto-increment, and the settings the driver writes.
*/
#ifndef FAKE_SH1106_H
#define FAKE_SH1106_H

#include <stddef.h>
#include <stdint.h>

#include "sh1106.h"

typedef struct
{
    int     present;
    uint8_t ram[SH1106_PAGES][SH1106_RAM_COLS];
    uint8_t page, col;
    int     display_on, dcdc_on, seg_remap, com_remap, multiplex;
    int     writes; /* I2C writes received */
} fake_sh1106_t;

extern fake_sh1106_t fake_sh1106;

void fake_sh1106_reset(void);
int  fake_sh1106_write(void *ctx, uint8_t addr, const uint8_t *data, size_t len);
/* A visible pixel (x 0..127, y 0..63) as the panel shows it */
int  fake_sh1106_pixel(unsigned x, unsigned y);

#endif /* FAKE_SH1106_H */

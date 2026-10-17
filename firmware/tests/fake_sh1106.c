/*
** Command-level model of an SH1106 OLED controller (see fake_sh1106.h), from the SH1106 datasheet
*/
#include "fake_sh1106.h"

#include <string.h>

fake_sh1106_t fake_sh1106;

void fake_sh1106_reset(void)
{
    memset(&fake_sh1106, 0, sizeof(fake_sh1106));
    fake_sh1106.present   = 1;
    fake_sh1106.multiplex = 0x3F;
    memset(fake_sh1106.ram, 0xA5, sizeof(fake_sh1106.ram)); /* the memory isn't cleared at power-on */
}

/* One command; returns how many parameter bytes it takes */
static size_t command(const uint8_t *c, size_t avail)
{
    fake_sh1106_t *F = &fake_sh1106;
    uint8_t        op = c[0];

    if (op <= 0x0F)
    {
        F->col = (uint8_t)((F->col & 0xF0) | op);
    }
    else if (op <= 0x1F)
    {
        F->col = (uint8_t)((F->col & 0x0F) | ((op & 0x0F) << 4));
    }
    else if ((op & 0xF8) == SH1106_CMD_PAGE)
    {
        F->page = op & 0x07;
    }
    else if (op == SH1106_CMD_DISPLAY_ON || op == SH1106_CMD_DISPLAY_OFF)
    {
        F->display_on = op == SH1106_CMD_DISPLAY_ON;
    }
    else if (op == SH1106_CMD_SEG_REMAP || op == 0xA0)
    {
        F->seg_remap = op == SH1106_CMD_SEG_REMAP;
    }
    else if (op == SH1106_CMD_COM_REMAP || op == 0xC0)
    {
        F->com_remap = op == SH1106_CMD_COM_REMAP;
    }
    else if (op == SH1106_CMD_DCDC || op == SH1106_CMD_MULTIPLEX || op == SH1106_CMD_CONTRAST ||
             op == SH1106_CMD_OFFSET || op == SH1106_CMD_CLOCK || op == SH1106_CMD_PRECHARGE ||
             op == SH1106_CMD_COM_PINS || op == SH1106_CMD_VCOM)
    {
        if (avail < 2)
        {
            return 0;
        }
        if (op == SH1106_CMD_DCDC)
        {
            F->dcdc_on = c[1] == 0x8B;
        }
        else if (op == SH1106_CMD_MULTIPLEX)
        {
            F->multiplex = c[1];
        }
        return 1;
    }
    return 0;
}

int fake_sh1106_write(void *ctx, uint8_t addr, const uint8_t *data, size_t len)
{
    fake_sh1106_t *F = &fake_sh1106;
    size_t         i;

    (void)ctx;
    if (!F->present || addr != 0x3C || len < 1)
    {
        return -1; /* no acknowledge */
    }
    F->writes++;
    if (data[0] == SH1106_CTRL_DATA)
    {
        for (i = 1; i < len; i++)
        {
            if (F->col < SH1106_RAM_COLS)
            {
                F->ram[F->page][F->col] = data[i];
            }
            F->col++; /* the column advances; the page doesn't */
        }
    }
    else if (data[0] == SH1106_CTRL_CMD)
    {
        for (i = 1; i < len; i++)
        {
            i += command(&data[i], len - i);
        }
    }
    else
    {
        return -1;
    }
    return 0;
}

int fake_sh1106_pixel(unsigned x, unsigned y)
{
    return (fake_sh1106.ram[y / 8][x + SH1106_COL_OFFSET] >> (y % 8)) & 1;
}

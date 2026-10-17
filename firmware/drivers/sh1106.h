/*
** Sino Wealth SH1106 128 x 64 OLED controller on I2C (the OBC's status display, address 0x3C)
**
** Portable: the I2C bus is a callback, so the same code runs on the Pico (I2C0, GP8 SDA, GP9 SCL) and in the unit
** tests against a model of the controller (tests/fake_sh1106.c).
**
** The SH1106 has 132 columns of display memory; the 128 visible ones start at column 2. It only has page
** addressing: each of the 8 pages is 8 pixel rows, one byte per column, least significant bit at the top. A frame
** is written one page at a time (sh1106_write_page, about 3 ms at 400 kHz), so a caller can spread a refresh over
** several scheduler slots.
**
** Text: a 5 x 7 font in 6-pixel cells, 21 characters on each of the 8 lines (sh1106_text, into a frame buffer).
*/
#ifndef SH1106_H
#define SH1106_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int (*write)(void *ctx, uint8_t addr, const uint8_t *data, size_t len); /* one I2C write; 0 on success */
    void   *ctx;
    uint8_t addr;
} sh1106_t;

#define SH1106_WIDTH 128
#define SH1106_PAGES 8
#define SH1106_COLS  21 /* text: characters per line */
#define SH1106_LINES 8  /* text: one line per page */

#define SH1106_OK      0
#define SH1106_ERR_I2C (-1) /* the write wasn't acknowledged: no display, or not powered */
#define SH1106_ERR_ARG (-2)

typedef uint8_t sh1106_frame_t[SH1106_PAGES][SH1106_WIDTH];

/* Configures the controller, clears its memory and turns the display on */
int sh1106_init(sh1106_t *d);
/* Writes one page (8 pixel rows) of the visible area */
int sh1106_write_page(sh1106_t *d, unsigned page, const uint8_t data[SH1106_WIDTH]);
/* Clears text line `line` of the frame and draws s on it (up to SH1106_COLS characters; others show as '?') */
void sh1106_text(sh1106_frame_t fb, unsigned line, const char *s);
/* The font's 5 columns for character c, for the tests */
const uint8_t *sh1106_glyph(char c);

/* Control bytes and commands (datasheet), for the driver and the controller model */
#define SH1106_CTRL_CMD        0x00 /* control byte: commands follow */
#define SH1106_CTRL_DATA       0x40 /* control byte: display data follows */
#define SH1106_CMD_COL_LOW     0x00 /* | low nibble of the column */
#define SH1106_CMD_COL_HIGH    0x10 /* | high nibble */
#define SH1106_CMD_PUMP_VOLT   0x30 /* | 0..3: 6.4, 7.4, 8.0, 9.0 V */
#define SH1106_CMD_START_LINE  0x40 /* | line */
#define SH1106_CMD_CONTRAST    0x81 /* + 1 byte */
#define SH1106_CMD_SEG_REMAP   0xA1 /* column 131 at SEG0: the image isn't mirrored on the usual breakouts */
#define SH1106_CMD_ALL_ON_OFF  0xA4 /* show the memory, not all pixels on */
#define SH1106_CMD_NORMAL      0xA6
#define SH1106_CMD_MULTIPLEX   0xA8 /* + 1 byte: rows - 1 */
#define SH1106_CMD_DCDC        0xAD /* + 1 byte: 0x8B = on */
#define SH1106_CMD_DISPLAY_OFF 0xAE
#define SH1106_CMD_DISPLAY_ON  0xAF
#define SH1106_CMD_PAGE        0xB0 /* | page */
#define SH1106_CMD_COM_REMAP   0xC8 /* scan from COM63 to COM0: not upside down */
#define SH1106_CMD_OFFSET      0xD3 /* + 1 byte */
#define SH1106_CMD_CLOCK       0xD5 /* + 1 byte */
#define SH1106_CMD_PRECHARGE   0xD9 /* + 1 byte */
#define SH1106_CMD_COM_PINS    0xDA /* + 1 byte */
#define SH1106_CMD_VCOM        0xDB /* + 1 byte */
#define SH1106_COL_OFFSET      2    /* the first visible column */
#define SH1106_RAM_COLS        132

#ifdef __cplusplus
}
#endif

#endif /* SH1106_H */

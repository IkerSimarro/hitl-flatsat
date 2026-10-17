/*
** The SX1262 on the Pico-LoRa-SX1262 board, bound to the Pico's pins (hardware/PINOUT.md): SPI1 (GP10 SCK,
** GP11 MOSI, GP12 MISO) at 8 MHz, CS GP3, BUSY GP2, RESET GP15. Used by the OBC and the ground modem.
*/
#ifndef RADIO_PICO_H
#define RADIO_PICO_H

#include "sx1262.h"

/* Sets up the pins and fills in the callbacks; the caller then sets the frequency and power and calls sx1262_init */
void pico_sx1262_bind(sx1262_t *radio);

#endif /* RADIO_PICO_H */

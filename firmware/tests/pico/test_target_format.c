/*
** On-target check of number formatting on the RP2350 (NCR-017): float values as the ADCS computes them,
** promoted to double and formatted with %.6e as the reaction wheel driver does, zero and negative zero (a
** wheel torque is the negative of a body torque), and double subnormals. Built twice: with the SDK's printf
** (test_target_format) and with the C library's (test_target_format_newlib, as the OBC now uses).
** Prints one line per value over USB serial, repeated every 2 s; read it from the host (/dev/ttyACM0).
*/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

static void show(const char *what, double d)
{
    uint64_t u;
    char     text[40];

    memcpy(&u, &d, sizeof(u));
    snprintf(text, sizeof(text), "%.6e", d);
    printf("%-26s bits %08lx%08lx  %%.6e -> %s\n", what, (unsigned long)(u >> 32), (unsigned long)u, text);
}

int main(void)
{
    static volatile float f[] = {-2.0303e-4f, 2.23517e-11f, 1e-30f, 1.17549435e-38f, 1e-39f, 1e-40f, 1.4e-45f, -1e-42f};
    static volatile float  z[] = {0.0f, -0.0f};
    static volatile double d[] = {2.2250738585072014e-308, 1e-310, 4.9e-324};
    char name[32];

    stdio_init_all();
    for (;;)
    {
        unsigned i;
        printf("---- float -> double -> %%.6e\n");
        for (i = 0; i < sizeof(f) / sizeof(f[0]); i++)
        {
            snprintf(name, sizeof(name), "float %.3g", (double)f[i]);
            show(name, (double)f[i]);
        }
        printf("---- zero (a float body torque of 0, negated for the wheel)\n");
        show("float +0", (double)z[0]);
        show("float -0", (double)z[1]);
        show("float -(+0)", (double)-z[0]);
        printf("---- double\n");
        for (i = 0; i < sizeof(d) / sizeof(d[0]); i++)
        {
            show("double subnormal", d[i]);
        }
        sleep_ms(2000);
    }
}

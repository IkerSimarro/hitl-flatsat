/*
** Status display: software-in-the-loop backend
**
** The text the OLED would show goes to $SIL_LOG_DIR/oled.txt, replaced at each update, so a SIL run's display can be
** watched (watch cat oled.txt) and checked. Without SIL_LOG_DIR there is no display. The Pico build drives the
** OLED instead, behind the same calls (hal/pico/dev_display_pico.c).
*/
#include <stdio.h>
#include <stdlib.h>

#include "dev_internal.h"

int dev_display_show(dev_display_text_t text)
{
    static char path[512], tmp[520];
    FILE       *f;
    unsigned    i;

    if (path[0] == '\0')
    {
        const char *dir = getenv("SIL_LOG_DIR");
        if (dir == NULL)
        {
            return DEV_ERR_BUS;
        }
        snprintf(path, sizeof(path), "%s/oled.txt", dir);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    }
    f = fopen(tmp, "w");
    if (f == NULL)
    {
        return DEV_ERR_BUS;
    }
    for (i = 0; i < DEV_DISPLAY_LINES; i++)
    {
        fprintf(f, "%s\n", text[i]);
    }
    fclose(f);
    return rename(tmp, path) == 0 ? DEV_OK : DEV_ERR_BUS; /* a reader never sees half a screen */
}

void dev_display_service(void)
{
}

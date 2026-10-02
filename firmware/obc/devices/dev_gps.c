/*
** GPS driver (novatel_oem615 over UART, NovAtel OEM6 ASCII logs)
**
** The receiver streams lines like
**   #BESTXYZA,COM1,0,55.0,FINESTEERING,<week>,<seconds>,...;<P-sol>,<pos type>,<X>,<Y>,<Z>,
**       <sigmas x3>,<V-sol>,<vel type>,<VX>,<VY>,<VZ>,...*<crc32 hex>\r\n
** The CRC is NovAtel's CRC-32 (reflected polynomial 0xEDB88320, initial 0) over the characters
** between '#' and '*'. Lines with a bad CRC are counted and dropped.
*/
#include <stdlib.h>
#include <string.h>

#include "dev_internal.h"

#define GPS_LINE_MAX   512
#define GPS_MAX_FIELDS 40

static char     line[GPS_LINE_MAX];
static size_t   line_len;
static int      line_overflow;
static uint32_t crc_errors;

static uint32_t novatel_crc32(const char *p, size_t n)
{
    uint32_t crc = 0;
    size_t   i;
    int      b;

    for (i = 0; i < n; i++)
    {
        crc ^= (uint8_t)p[i];
        for (b = 0; b < 8; b++)
        {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return crc;
}

/* Splits s in place at any of the separators; returns the number of fields */
static int split(char *s, const char *seps, char **fields, int max)
{
    int n = 0;

    fields[n++] = s;
    for (; *s != '\0' && n < max; s++)
    {
        if (strchr(seps, *s) != NULL)
        {
            *s          = '\0';
            fields[n++] = s + 1;
        }
    }
    return n;
}

static int parse_double(const char *s, double *out)
{
    char *end;

    *out = strtod(s, &end);
    return end != s;
}

/* Parses one complete line (without the line ending); returns 1 on a valid fix, 0 if not BESTXYZA, < 0 if bad */
static int parse_line(char *s, dev_gps_t *out)
{
    char    *star;
    char    *fields[GPS_MAX_FIELDS];
    char    *body;
    int      n;
    int      i;
    uint32_t crc;
    double   week;

    if (strncmp(s, "#BESTXYZA,", 10) != 0)
    {
        return 0;
    }
    star = strchr(s, '*');
    if (star == NULL)
    {
        return DEV_ERR_FORMAT;
    }
    crc = (uint32_t)strtoul(star + 1, NULL, 16);
    if (novatel_crc32(s + 1, (size_t)(star - (s + 1))) != crc)
    {
        crc_errors++;
        return DEV_ERR_FORMAT;
    }
    *star = '\0';

    body = strchr(s, ';');
    if (body == NULL)
    {
        return DEV_ERR_FORMAT;
    }
    *body++ = '\0';

    /* Header: BESTXYZA, port, sequence, idle, time status, week, seconds, ... */
    n = split(s + 1, ",", fields, GPS_MAX_FIELDS);
    if (n < 7 || !parse_double(fields[5], &week) || !parse_double(fields[6], &out->seconds_of_week))
    {
        return DEV_ERR_FORMAT;
    }
    out->week = (uint16_t)week;

    /* Body: P-sol, pos type, X, Y, Z, 3 sigmas, V-sol, vel type, VX, VY, VZ, ... */
    n = split(body, ",", fields, GPS_MAX_FIELDS);
    if (n < 13)
    {
        return DEV_ERR_FORMAT;
    }
    for (i = 0; i < 3; i++)
    {
        if (!parse_double(fields[2 + i], &out->pos[i]) || !parse_double(fields[10 + i], &out->vel[i]))
        {
            return DEV_ERR_FORMAT;
        }
    }
    return 1;
}

int dev_gps_init(void)
{
    line_len      = 0;
    line_overflow = 0;
    crc_errors    = 0;
    return dev_map_umb(fs_umb_uart_open(DEV_GPS_UART));
}

int dev_gps_poll(dev_gps_t *out)
{
    uint8_t buf[128];
    size_t  n;
    size_t  i;
    int     result = 0;

    while ((n = fs_umb_uart_read(DEV_GPS_UART, buf, sizeof(buf))) > 0)
    {
        for (i = 0; i < n; i++)
        {
            char c = (char)buf[i];

            if (c == '#')
            {
                /* Start of a log: anything before it is noise or a truncated line */
                line_len      = 0;
                line_overflow = 0;
            }
            if (c == '\n' || c == '\r')
            {
                if (line_len > 0 && !line_overflow)
                {
                    dev_gps_t fix;
                    int       rc;

                    line[line_len] = '\0';
                    rc             = parse_line(line, &fix);
                    if (rc == 1)
                    {
                        *out   = fix; /* the latest valid log wins */
                        result = 1;
                    }
                    else if (rc < 0 && result != 1)
                    {
                        result = rc;
                    }
                }
                line_len = 0;
                continue;
            }
            if (line_len < sizeof(line) - 1)
            {
                line[line_len++] = c;
            }
            else
            {
                line_overflow = 1;
            }
        }
    }
    return result;
}

uint32_t dev_gps_crc_errors(void)
{
    return crc_errors;
}

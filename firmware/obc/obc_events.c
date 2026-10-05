/*
** Event service: EVENT telemetry packets plus a log line for each event
*/
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fs_ccsds.h"
#include "fs_hal.h"
#include "fs_umbilical.h"
#include "obc.h"

static const char *severity_name(uint8_t s)
{
    static const char *names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

void obc_event(uint16_t id, uint8_t severity, const char *fmt, ...)
{
    flatsat_event_t ev;
    uint8_t         pkt[FLATSAT_EVENT_LEN];
    fs_time_t       now = fs_time_now();
    va_list         ap;
    size_t          n;

    memset(&ev, 0, sizeof(ev));
    ev.event_id = id;
    ev.severity = severity;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);

    obc.event_count++;
    fs_hal_log("EVENT %u %s: %s", id, severity_name(severity), ev.text);

    n = fs_tlm_build(pkt, sizeof(pkt), FLATSAT_EVENT_MID, now.seconds, now.subseconds, &ev, sizeof(ev));
    if (n > 0)
    {
        obc_tlm_send_packet(pkt, n);
        obc_comms_queue(pkt, n); /* events go down in the next contact (ICD 4) */
    }
}

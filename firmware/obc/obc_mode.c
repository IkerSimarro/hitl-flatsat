/*
** Mode manager (ICD 5.1)
**
** Allowed transitions: SAFE can go anywhere; the operational modes (DETUMBLE, SUN_POINT) can go to each
** other, to SAFE, LOW_POWER or TEST; LOW_POWER and TEST can only return to SAFE. Entering SAFE is
** always allowed, so faults and operators can always stop the actuators.
*/
#include "obc.h"

#define NUM_MODES 5

/* allowed[from][to] */
static const uint8_t allowed[NUM_MODES][NUM_MODES] = {
    /*               SAFE DETUMBLE SUN_POINT LOW_POWER TEST */
    /* SAFE      */ {1, 1, 1, 1, 1},
    /* DETUMBLE  */ {1, 1, 1, 1, 1},
    /* SUN_POINT */ {1, 1, 1, 1, 1},
    /* LOW_POWER */ {1, 0, 0, 1, 0},
    /* TEST      */ {1, 0, 0, 0, 1},
};

const char *obc_mode_name(uint8_t mode)
{
    static const char *names[NUM_MODES] = {"SAFE", "DETUMBLE", "SUN_POINT", "LOW_POWER", "TEST"};
    return mode < NUM_MODES ? names[mode] : "INVALID";
}

void obc_mode_init(void)
{
    obc.mode        = FLATSAT_MODE_SAFE;
    obc.mode_reason = FLATSAT_MODE_REASON_BOOT;
}

int obc_mode_request(uint8_t mode, uint8_t reason)
{
    uint8_t from = obc.mode;
    int     i;

    if (mode >= NUM_MODES || !allowed[from][mode])
    {
        obc_event(EVT_MODE_REFUSED, FLATSAT_SEVERITY_WARNING, "mode %s -> %s not allowed", obc_mode_name(from),
                  obc_mode_name(mode));
        return 0;
    }
    if (mode == from)
    {
        return 1;
    }

    /* Leaving an actuating mode: stop the actuators. ADCS control laws are added in Phase 2. */
    if (mode == FLATSAT_MODE_SAFE || mode == FLATSAT_MODE_LOW_POWER)
    {
        for (i = 0; i < DEV_NUM_TRQ; i++)
        {
            dev_trq_set((uint8_t)i, 0.0f);
            obc.trq_duty[i] = 0;
        }
        for (i = 0; i < DEV_NUM_RW; i++)
        {
            dev_rw_set_torque((uint8_t)i, 0.0);
        }
    }

    obc.mode        = mode;
    obc.mode_reason = reason;
    obc_event(EVT_MODE_CHANGE, FLATSAT_SEVERITY_INFO, "mode %s -> %s (reason %u)", obc_mode_name(from),
              obc_mode_name(mode), reason);
    return 1;
}

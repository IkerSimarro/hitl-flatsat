/*
** Fault persistence filter (see fs_persist.h)
*/
#include "fs_persist.h"

void fs_persist_init(fs_persist_t *p, uint8_t threshold)
{
    p->threshold   = threshold > 0 ? threshold : 1;
    p->consecutive = 0;
    p->faulted     = 0;
    p->misses      = 0;
}

fs_persist_event_t fs_persist_update(fs_persist_t *p, int ok)
{
    if (ok)
    {
        p->consecutive = 0;
        if (p->faulted)
        {
            p->faulted = 0;
            return FS_PERSIST_CLEARED;
        }
        return FS_PERSIST_NO_CHANGE;
    }

    p->misses++;
    if (p->consecutive < p->threshold)
    {
        p->consecutive++;
    }
    if (!p->faulted && p->consecutive >= p->threshold)
    {
        p->faulted = 1;
        return FS_PERSIST_TRIPPED;
    }
    return FS_PERSIST_NO_CHANGE;
}

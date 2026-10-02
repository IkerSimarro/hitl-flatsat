/*
** Fault persistence filter (FDIR)
**
** A fault is declared only after `threshold` consecutive failed checks, so isolated misses (a late reply
** on a loaded bus) don't raise fault events; it is cleared on the first good check after that. Callers
** still treat each failed check's data as invalid; this only decides when to report a fault.
*/
#ifndef FS_PERSIST_H
#define FS_PERSIST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    FS_PERSIST_NO_CHANGE = 0,
    FS_PERSIST_TRIPPED,  /* this check declared the fault */
    FS_PERSIST_CLEARED   /* this check cleared a declared fault */
} fs_persist_event_t;

typedef struct
{
    uint8_t  threshold;   /* consecutive failures needed to declare a fault (>= 1) */
    uint8_t  consecutive; /* current run of failures, saturating at threshold */
    uint8_t  faulted;
    uint32_t misses;      /* every failed check, including ones that didn't trip */
} fs_persist_t;

void               fs_persist_init(fs_persist_t *p, uint8_t threshold);
fs_persist_event_t fs_persist_update(fs_persist_t *p, int ok);

#ifdef __cplusplus
}
#endif

#endif /* FS_PERSIST_H */

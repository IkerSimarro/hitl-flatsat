/*
** Quadrature decoding of the wheel encoder's two Hall channels (the ADCS node, hal/pico/dev_motor_pico.c)
**
** Called on every edge of either channel with both levels. One count per edge (four per encoder pulse), up when
** A leads B. A change of both channels at once means an edge was missed: the step's direction is unknown, so it
** is counted as an error, not as a step. Plain logic, so it is unit-tested on Linux (tests/test_drivers.c).
*/
#ifndef QUADRATURE_H
#define QUADRATURE_H

#include <stdint.h>

typedef struct
{
    uint8_t  state;  /* A << 1 | B */
    int32_t  count;  /* wraps */
    uint32_t errors; /* missed edges */
} quad_t;

static inline void quad_init(quad_t *q, int a, int b)
{
    q->state  = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
    q->count  = 0;
    q->errors = 0;
}

static inline void quad_update(quad_t *q, int a, int b)
{
    /* [previous << 2 | next]: +1 forward (A leads: 00 10 11 01), -1 backward, 0 no change, 2 both changed */
    static const int8_t step[16] = {0, -1, 1, 2, 1, 0, 2, -1, -1, 2, 0, 1, 2, 1, -1, 0};
    uint8_t             next     = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
    int8_t              s        = step[(q->state << 2) | next];

    if (s == 2)
    {
        q->errors++;
    }
    else
    {
        q->count = (int32_t)((uint32_t)q->count + (uint32_t)(int32_t)s); /* wraps without overflow */
    }
    q->state = next;
}

#endif /* QUADRATURE_H */

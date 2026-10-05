/*
** SIL harness in POSIX shared memory (see sil_harness.h)
*/
#include "sil_harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define HARNESS_NAME  "/flatsat-harness"
#define HARNESS_MAGIC 0xF1A75A7u

sil_harness_t *sil_harness(void)
{
    static sil_harness_t *h;
    int                   fd;

    if (h != NULL)
    {
        return h;
    }
    fd = shm_open(HARNESS_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0 || ftruncate(fd, sizeof(sil_harness_t)) != 0)
    {
        perror("sil_harness: shm_open");
        if (fd >= 0)
        {
            close(fd);
        }
        return NULL;
    }
    h = mmap(NULL, sizeof(sil_harness_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (h == MAP_FAILED)
    {
        perror("sil_harness: mmap");
        h = NULL;
        return NULL;
    }
    /* First process to map it initialises the "power-on" state; the lines default to released */
    if (__sync_bool_compare_and_swap(&h->magic, 0, HARNESS_MAGIC))
    {
        h->adcs_run     = 1;
        h->motor_nsleep = 1;
        h->motor_duty   = 0.0f;
        h->radio_ma     = 0.0f;
        h->usb_power    = 0;
        h->battery_v    = 3.9f; /* until the plant model runs */
    }
    return h;
}

/*
** FlatSat HAL, Linux backend (software-in-the-loop)
**
** Options, read from the node's command line (other arguments are left for the node):
**   --umb-pty LINK   create a pty pair and symlink the far end to LINK for the HIL bridge to open
**   --umb-dev PATH   open an existing serial device for the umbilical instead
**   --can IFACE      SocketCAN interface for the FlatSat bus (default vcan0, "none" to disable)
**
** fs_hal_reboot() re-executes the node with the same arguments, passing the reset cause in the
** environment, so reboot handling behaves as it will on the Pico.
*/
#define _GNU_SOURCE

#include "fs_hal.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pty.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "flatsat_icd.h"

#define RESET_CAUSE_ENV "FS_RESET_CAUSE"

static int    umb_fd   = -1;
static int    umb_peer = -1; /* far end of our pty, kept open so the pty survives bridge restarts */
static int    can_fd   = -1;
static char **saved_argv;
static uint64_t start_ns;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static const char *find_option(int argc, char **argv, const char *name)
{
    int i;

    for (i = 1; i + 1 < argc; i++)
    {
        if (strcmp(argv[i], name) == 0)
        {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int open_umb_pty(const char *link)
{
    struct termios tio;
    int            peer;

    if (openpty(&umb_fd, &peer, NULL, NULL, NULL) != 0)
    {
        fs_hal_log("umbilical: openpty failed: %s", strerror(errno));
        return -1;
    }
    /* Raw mode on both ends, so no byte is translated or echoed */
    tcgetattr(peer, &tio);
    cfmakeraw(&tio);
    tcsetattr(peer, TCSANOW, &tio);
    tcgetattr(umb_fd, &tio);
    cfmakeraw(&tio);
    tcsetattr(umb_fd, TCSANOW, &tio);
    fcntl(umb_fd, F_SETFL, fcntl(umb_fd, F_GETFL) | O_NONBLOCK);

    unlink(link);
    if (symlink(ttyname(peer), link) != 0)
    {
        fs_hal_log("umbilical: cannot create %s: %s", link, strerror(errno));
        return -1;
    }
    umb_peer = peer;
    fs_hal_log("umbilical: bridge side is %s -> %s", link, ttyname(peer));
    return 0;
}

static int open_umb_dev(const char *path)
{
    struct termios tio;

    umb_fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (umb_fd < 0)
    {
        fs_hal_log("umbilical: cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (tcgetattr(umb_fd, &tio) == 0)
    {
        cfmakeraw(&tio);
        tcsetattr(umb_fd, TCSANOW, &tio);
    }
    return 0;
}

static int open_can(const char *iface)
{
    struct sockaddr_can addr;
    struct ifreq        ifr;

    can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (can_fd < 0)
    {
        fs_hal_log("CAN: socket failed: %s", strerror(errno));
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    if (ioctl(can_fd, SIOCGIFINDEX, &ifr) < 0)
    {
        fs_hal_log("CAN: interface %s not found (create it with: ip link add dev %s type vcan; ip link set up %s)",
                   iface, iface, iface);
        close(can_fd);
        can_fd = -1;
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(can_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fs_hal_log("CAN: bind to %s failed: %s", iface, strerror(errno));
        close(can_fd);
        can_fd = -1;
        return -1;
    }
    fcntl(can_fd, F_SETFL, fcntl(can_fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

int fs_hal_init(int argc, char **argv)
{
    const char *umb_pty = find_option(argc, argv, "--umb-pty");
    const char *umb_dev = find_option(argc, argv, "--umb-dev");
    const char *can     = find_option(argc, argv, "--can");
    int         rc      = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    start_ns   = mono_ns();
    saved_argv = argv;

    if (umb_pty != NULL)
    {
        rc |= open_umb_pty(umb_pty);
    }
    else if (umb_dev != NULL)
    {
        rc |= open_umb_dev(umb_dev);
    }

    if (can == NULL)
    {
        can = "vcan0";
    }
    if (strcmp(can, "none") != 0)
    {
        rc |= open_can(can);
    }
    return rc;
}

/* ---- Time ---- */

uint64_t fs_hal_time_us(void)
{
    return (mono_ns() - start_ns) / 1000u;
}

void fs_hal_sleep_us(uint32_t us)
{
    struct timespec ts;
    ts.tv_sec  = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
}

/* ---- Umbilical ---- */

int fs_hal_umb_write(const uint8_t *data, size_t len)
{
    size_t off = 0;

    if (umb_fd < 0)
    {
        return -1;
    }
    while (off < len)
    {
        ssize_t w = write(umb_fd, data + off, len - off);
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN)
            {
                /* Bridge not reading fast enough: wait briefly rather than drop a partial frame */
                fs_hal_sleep_us(100);
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return (int)len;
}

int fs_hal_umb_read(uint8_t *buf, size_t max)
{
    ssize_t n;

    if (umb_fd < 0)
    {
        return 0;
    }
    n = read(umb_fd, buf, max);
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EINTR || errno == EIO) ? 0 : -1;
    }
    return (int)n;
}

/* ---- CAN ---- */

int fs_hal_can_send(const fs_can_frame_t *frame)
{
    struct can_frame cf;

    if (can_fd < 0)
    {
        return -1;
    }
    memset(&cf, 0, sizeof(cf));
    cf.can_id  = frame->id & CAN_SFF_MASK;
    cf.can_dlc = frame->dlc > 8 ? 8 : frame->dlc;
    memcpy(cf.data, frame->data, cf.can_dlc);
    return write(can_fd, &cf, sizeof(cf)) == (ssize_t)sizeof(cf) ? 0 : -1;
}

int fs_hal_can_recv(fs_can_frame_t *frame)
{
    struct can_frame cf;
    ssize_t          n;

    if (can_fd < 0)
    {
        return 0;
    }
    n = read(can_fd, &cf, sizeof(cf));
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    }
    if (n != (ssize_t)sizeof(cf) || (cf.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)))
    {
        return 0; /* only classic 11-bit data frames are part of the FlatSat protocol */
    }
    frame->id  = (uint16_t)(cf.can_id & CAN_SFF_MASK);
    frame->dlc = cf.can_dlc;
    memcpy(frame->data, cf.data, 8);
    return 1;
}

void fs_hal_can_error_counters(uint8_t *tec, uint8_t *rec)
{
    *tec = 0;
    *rec = 0;
}

/* ---- System ---- */

uint8_t fs_hal_reset_cause(void)
{
    const char *cause = getenv(RESET_CAUSE_ENV);
    return cause != NULL ? (uint8_t)atoi(cause) : FLATSAT_RESET_CAUSE_POWER_ON;
}

void fs_hal_reboot(void)
{
    char cause[8];

    fs_hal_log("rebooting");
    snprintf(cause, sizeof(cause), "%d", FLATSAT_RESET_CAUSE_COMMAND);
    setenv(RESET_CAUSE_ENV, cause, 1);
    if (umb_fd >= 0)
    {
        close(umb_fd);
    }
    if (umb_peer >= 0)
    {
        close(umb_peer);
    }
    if (can_fd >= 0)
    {
        close(can_fd);
    }
    execv("/proc/self/exe", saved_argv);
    fs_hal_log("reboot failed: %s", strerror(errno));
    exit(1);
}

void fs_hal_watchdog_kick(void)
{
}

void fs_hal_log(const char *fmt, ...)
{
    va_list  ap;
    uint64_t ms = fs_hal_time_us() / 1000u;

    printf("[%6llu.%03llu] %s: ", (unsigned long long)(ms / 1000u), (unsigned long long)(ms % 1000u),
           program_invocation_short_name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/*
** vcan_up: create a Linux virtual CAN interface (if missing) and bring it up
**
** The SIL environment uses vcan0 as the FlatSat CAN bus between the node processes. The NOS3 image has
** no iproute2, so this does what "ip link add dev vcan0 type vcan; ip link set up vcan0" does, through
** rtnetlink. Needs the vcan kernel module (loaded on the host) and CAP_NET_ADMIN.
**
** Usage: vcan_up [IFNAME]   (default vcan0)
*/
#define _DEFAULT_SOURCE

#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#define BUF_SIZE 512

static struct rtattr *add_attr(struct nlmsghdr *n, unsigned short type, const void *data, size_t len)
{
    struct rtattr *rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));

    rta->rta_type = type;
    rta->rta_len  = (unsigned short)RTA_LENGTH(len);
    if (len > 0)
    {
        memcpy(RTA_DATA(rta), data, len);
    }
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    return rta;
}

/* Returns 0 if created or already present, -errno otherwise */
static int create_vcan(const char *ifname)
{
    char              buf[BUF_SIZE];
    struct nlmsghdr  *n = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi;
    struct rtattr    *linkinfo;
    struct sockaddr_nl sa;
    ssize_t           len;
    int               fd;
    int               rc;

    memset(buf, 0, sizeof(buf));
    n->nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    n->nlmsg_type  = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    ifi            = NLMSG_DATA(n);
    ifi->ifi_family = AF_UNSPEC;

    add_attr(n, IFLA_IFNAME, ifname, strlen(ifname) + 1);
    linkinfo = add_attr(n, IFLA_LINKINFO, NULL, 0);
    add_attr(n, IFLA_INFO_KIND, "vcan", strlen("vcan"));
    linkinfo->rta_len = (unsigned short)((char *)n + n->nlmsg_len - (char *)linkinfo);

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
    {
        return -errno;
    }
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (sendto(fd, n, n->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        rc = -errno;
        close(fd);
        return rc;
    }

    len = recv(fd, buf, sizeof(buf), 0);
    close(fd);
    if (len < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)) || n->nlmsg_type != NLMSG_ERROR)
    {
        return -EPROTO;
    }
    rc = ((struct nlmsgerr *)NLMSG_DATA(n))->error; /* 0 = success, else -errno */
    return rc == -EEXIST ? 0 : rc;
}

static int set_up(const char *ifname)
{
    struct ifreq ifr;
    int          fd = socket(AF_INET, SOCK_DGRAM, 0);
    int          rc = 0;

    if (fd < 0)
    {
        return -errno;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
    {
        rc = -errno;
    }
    else if (!(ifr.ifr_flags & IFF_UP))
    {
        ifr.ifr_flags |= IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
        {
            rc = -errno;
        }
    }
    close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    const char *ifname = argc > 1 ? argv[1] : "vcan0";
    int         rc     = create_vcan(ifname);

    if (rc != 0)
    {
        fprintf(stderr, "vcan_up: cannot create %s: %s%s\n", ifname, strerror(-rc),
                rc == -EOPNOTSUPP || rc == -ENOENT ? " (is the vcan kernel module loaded on the host?)" :
                rc == -EPERM ? " (needs CAP_NET_ADMIN)" : "");
        return 1;
    }
    rc = set_up(ifname);
    if (rc != 0)
    {
        fprintf(stderr, "vcan_up: cannot bring %s up: %s\n", ifname, strerror(-rc));
        return 1;
    }
    printf("vcan_up: %s is up\n", ifname);
    return 0;
}

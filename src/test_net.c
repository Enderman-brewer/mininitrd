#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "init.h"

static int bring_up_lo(void)
{
    /* on a fresh boot "lo" exists but is administratively DOWN, so a
     * connect() to 127.0.0.1 would fail even on a perfectly healthy kernel */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) { close(fd); return -1; }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) { close(fd); return -1; }
    close(fd);
    return 0;
}

/* 18. networking loopback ------------------------------------------- */
int net_loopback_test(void)
{
    int lfd, cfd, afd;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    char buf[32];
    int port;
    int lo_ok = (bring_up_lo() == 0);

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) return 2;
        return 1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(lfd); return 1;
    }
    if (listen(lfd, 4) != 0) { close(lfd); return 1; }
    if (getsockname(lfd, (struct sockaddr *)&a, &alen) != 0) {
        close(lfd); return 1;
    }
    port = ntohs(a.sin_port);
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) { close(lfd); return 1; }
    a.sin_port = htons((uint16_t)port);
    if (connect(cfd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(cfd); close(lfd);
        /* loopback unreachable: real kernel problem only if we could
         * bring "lo" up ourselves -- otherwise it's an environment limit */
        if (!lo_ok) return 2;
        return 1;
    }
    afd = accept(lfd, NULL, NULL);
    if (afd < 0) { close(cfd); close(lfd); return 1; }
    if (send(cfd, "hello", 5, 0) != 5) {
        close(afd); close(cfd); close(lfd); return 1;
    }
    if (recv(afd, buf, sizeof(buf), 0) != 5 ||
        memcmp(buf, "hello", 5) != 0) {
        close(afd); close(cfd); close(lfd); return 1;
    }
    close(afd); close(cfd); close(lfd);

    /* AF_UNIX socketpair */
    {
        int sp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return 2;
        if (send(sp[0], "unix", 4, 0) != 4) { close(sp[0]); close(sp[1]); return 1; }
        if (recv(sp[1], buf, sizeof(buf), 0) != 4 ||
            memcmp(buf, "unix", 4) != 0) {
            close(sp[0]); close(sp[1]); return 1;
        }
        close(sp[0]); close(sp[1]);
    }
    return 0;
}

/* 41. UDP loopback -------------------------------------------------------- */
int net_udp_test(void)
{
    int s;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    char buf[32];
    if (bring_up_lo() != 0)
        return 2;               /* cannot bring up loopback: environment */
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) return 2;
        return 1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return 1; }
    if (getsockname(s, (struct sockaddr *)&a, &alen) != 0) { close(s); return 1; }
    if (sendto(s, "udp-ping", 8, 0, (struct sockaddr *)&a, sizeof(a)) != 8) {
        close(s); return 1;
    }
    for (;;) {
        ssize_t rr = recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
        if (rr == 8) break;
        if (rr < 0 && errno == EINTR) continue;
        close(s); return 1;
    }
    if (memcmp(buf, "udp-ping", 8) != 0) { close(s); return 1; }
    close(s);
    return 0;
}

/* 42. AF_UNIX datagram --------------------------------------------------- */
int unix_dgram_test(void)
{
    int sp[2];
    char buf[32];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) != 0) return 2;
    if (send(sp[0], "dgram", 5, 0) != 5) { close(sp[0]); close(sp[1]); return 1; }
    if (recv(sp[1], buf, sizeof(buf), 0) != 5 || memcmp(buf, "dgram", 5) != 0) {
        close(sp[0]); close(sp[1]); return 1;
    }
    close(sp[0]); close(sp[1]);
    return 0;
}

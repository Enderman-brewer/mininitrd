#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>

#include "init.h"

/* 23. epoll ----------------------------------------------------------- */
int epoll_test(void)
{
    int efd, fds[2], n;
    struct epoll_event ev;
    char c;
    if (pipe(fds) != 0) return 1;
    efd = epoll_create1(0);
    if (efd < 0) {
        close(fds[0]); close(fds[1]);
        if (errno == ENOSYS || errno == EINVAL) return 2;
        return 1;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fds[0];
    if (epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
        close(efd); close(fds[0]); close(fds[1]); return 1;
    }
    n = epoll_wait(efd, &ev, 1, 20);
    if (n != 0) { close(efd); close(fds[0]); close(fds[1]); return 1; }
    if (write(fds[1], "x", 1) != 1) { close(efd); close(fds[0]); close(fds[1]); return 1; }
    n = epoll_wait(efd, &ev, 1, 1000);
    if (n != 1 || !(ev.events & EPOLLIN)) { close(efd); close(fds[0]); close(fds[1]); return 1; }
    if (read(fds[0], &c, 1) != 1 || c != 'x') { close(efd); close(fds[0]); close(fds[1]); return 1; }
    n = epoll_wait(efd, &ev, 1, 20);      /* drained: should time out */
    if (n != 0) { close(efd); close(fds[0]); close(fds[1]); return 1; }
    close(efd); close(fds[0]); close(fds[1]);
    return 0;
}

/* 24. eventfd ---------------------------------------------------------- */
int eventfd_test(void)
{
    int efd = eventfd(0, 0);
    uint64_t v = 5, got = 0;
    if (efd < 0) {
        if (errno == ENOSYS || errno == EINVAL) return 2;
        return 1;
    }
    if (write(efd, &v, sizeof(v)) != (ssize_t)sizeof(v)) { close(efd); return 1; }
    for (;;) {
        ssize_t rr = read(efd, &got, sizeof(got));
        if (rr == (ssize_t)sizeof(got)) break;
        if (rr < 0 && errno == EINTR) continue;
        close(efd); return 1;
    }
    if (got != 5) { close(efd); return 1; }
    close(efd);
    return 0;
}

/* 37. poll / select ----------------------------------------------------- */
int poll_select_test(void)
{
    int fds[2], n;
    struct pollfd pf[1];
    fd_set rfds;
    struct timeval tv;
    char c;
    if (pipe(fds) != 0) return 1;
    pf[0].fd = fds[0];
    pf[0].events = POLLIN;
    pf[0].revents = 0;
    n = poll(pf, 1, 30);
    if (n != 0) { close(fds[0]); close(fds[1]); return 1; }
    if (write(fds[1], "x", 1) != 1) { close(fds[0]); close(fds[1]); return 1; }
    n = poll(pf, 1, 1000);
    if (n != 1 || !(pf[0].revents & POLLIN)) { close(fds[0]); close(fds[1]); return 1; }
    FD_ZERO(&rfds);
    FD_SET(fds[0], &rfds);
    tv.tv_sec = 1; tv.tv_usec = 0;
    n = select(fds[0] + 1, &rfds, NULL, NULL, &tv);
    if (n != 1 || !FD_ISSET(fds[0], &rfds)) { close(fds[0]); close(fds[1]); return 1; }
    if (read(fds[0], &c, 1) != 1 || c != 'x') { close(fds[0]); close(fds[1]); return 1; }
    close(fds[0]); close(fds[1]);
    return 0;
}

/* 36. fd duplication: dup / dup2 / dup3 -------------------------------- */
int fd_dup_test(void)
{
    int fds[2], d1, d2, d3, flags, slot;
    if (pipe(fds) != 0) return 1;
    d1 = dup(fds[0]);
    if (d1 < 0) { close(fds[0]); close(fds[1]); return 1; }
    slot = fcntl(fds[0], F_DUPFD, 3);   /* lowest free fd >= 3 */
    if (slot < 0) { close(fds[0]); close(fds[1]); close(d1); return 1; }
    close(slot);                        /* now guaranteed free */
    d2 = dup2(fds[1], slot);
    if (d2 != slot) {
        close(fds[0]); close(fds[1]); close(d1); return 1;
    }
    slot = fcntl(fds[0], F_DUPFD, 3);
    if (slot < 0) { close(fds[0]); close(fds[1]); close(d1); close(d2); return 1; }
    close(slot);
    d3 = dup3(fds[0], slot, O_CLOEXEC);
    if (d3 != slot) {
        close(fds[0]); close(fds[1]); close(d1); close(d2);
        if (errno == ENOSYS) return 2;   /* ancient kernel without dup3 */
        return 1;
    }
    flags = fcntl(d3, F_GETFD);
    if (flags < 0 || !(flags & FD_CLOEXEC)) {
        close(fds[0]); close(fds[1]); close(d1); close(d2); close(d3);
        return 1;
    }
    close(fds[0]); close(fds[1]); close(d1); close(d2); close(d3);
    return 0;
}

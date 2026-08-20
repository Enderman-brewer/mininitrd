#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/signalfd.h>

#include "init.h"

static volatile sig_atomic_t g_sig_usr1;
static volatile sig_atomic_t g_sig_alrm;
static void on_usr1(int s) { (void)s; g_sig_usr1 = 1; }
static void on_alrm(int s) { (void)s; g_sig_alrm = 1; }

/* 14. signals ------------------------------------------------------- */
int signal_test(void)
{
    struct sigaction sa;
    struct itimerval it;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) return 1;
    g_sig_usr1 = 0;
    if (raise(SIGUSR1) != 0) return 1;
    if (!g_sig_usr1) return 1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) != 0) return 1;
    g_sig_alrm = 0;
    it.it_value.tv_sec = 0; it.it_value.tv_usec = 20000;
    it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 20000;
    if (setitimer(ITIMER_REAL, &it, NULL) != 0) return 1;
    clock_nanosleep(CLOCK_MONOTONIC, 0,
                    &(struct timespec){0, 80000000}, NULL);
    if (!g_sig_alrm) return 1;
    it.it_value.tv_sec = 0; it.it_value.tv_usec = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    return 0;
}

/* 26. signalfd ---------------------------------------------------------- */
int signalfd_test(void)
{
    sigset_t mask, old;
    int sfd;
    struct signalfd_siginfo si;
    int r = 1;
    if (sigemptyset(&mask) != 0) return 1;
    if (sigaddset(&mask, SIGUSR2) != 0) return 1;
    if (sigprocmask(SIG_BLOCK, &mask, &old) != 0) return 1;
    sfd = signalfd(-1, &mask, 0);
    if (sfd < 0) {
        if (errno == ENOSYS || errno == EINVAL) r = 2;
        sigprocmask(SIG_SETMASK, &old, NULL);
        return r;
    }
    if (raise(SIGUSR2) != 0)
        goto out;
    for (;;) {
        ssize_t rr = read(sfd, &si, sizeof(si));
        if (rr == (ssize_t)sizeof(si)) break;
        if (rr < 0 && errno == EINTR) continue;
        goto out;
    }
    if (si.ssi_signo == SIGUSR2)
        r = 0;
out:
    close(sfd);
    /* drain any pending SIGUSR2 before unblocking so it cannot kill us */
    sigtimedwait(&mask, NULL, &(struct timespec){0, 0});
    sigprocmask(SIG_SETMASK, &old, NULL);
    return r;
}

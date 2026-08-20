#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/timerfd.h>

#include "init.h"

/* 13. timers / clocks ------------------------------------------------ */
int timer_test(void)
{
    struct timespec t1, t2;
    struct timespec req, rem;
    double elapsed;

    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) return 1;
    if (clock_gettime(CLOCK_REALTIME, &t2) != 0) return 1;
    if (t2.tv_sec < 1) return 1;         /* wall clock must be sane */

    req.tv_sec = 0;
    req.tv_nsec = 50000000;              /* 50 ms */
    if (clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem) != 0) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &t2) != 0) return 1;
    elapsed = (double)(t2.tv_sec - t1.tv_sec) +
              (double)(t2.tv_nsec - t1.tv_nsec) / 1e9;
    logts("  50ms sleep took %.1f ms\n", elapsed * 1000.0);
    if (elapsed < 0.030 || elapsed > 2.0) return 1;

    /* hrtimer via timer_create (POSIX interval timer) */
    {
        timer_t tid;
        struct sigevent sev;
        struct itimerspec its;
        memset(&sev, 0, sizeof(sev));
        sev.sigev_notify = SIGEV_NONE;
        if (timer_create(CLOCK_MONOTONIC, &sev, &tid) != 0) return 2;
        its.it_value.tv_sec = 0; its.it_value.tv_nsec = 10000000;
        its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 10000000;
        if (timer_settime(tid, 0, &its, NULL) != 0) {
            timer_delete(tid); return 2;
        }
        clock_nanosleep(CLOCK_MONOTONIC, 0,
                        &(struct timespec){0, 30000000}, NULL);
        if (timer_gettime(tid, &its) != 0) { timer_delete(tid); return 1; }
        if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
            timer_delete(tid); return 1;
        }
        timer_delete(tid);
    }
    return 0;
}

/* 25. timerfd ----------------------------------------------------------- */
int timerfd_test(void)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its;
    uint64_t exp = 0;
    if (tfd < 0) {
        if (errno == ENOSYS || errno == EINVAL) return 2;
        return 1;
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 20000000;           /* one-shot, 20 ms */
    if (timerfd_settime(tfd, 0, &its, NULL) != 0) { close(tfd); return 1; }
    for (;;) {
        ssize_t rr = read(tfd, &exp, sizeof(exp));
        if (rr == (ssize_t)sizeof(exp)) break;
        if (rr < 0 && errno == EINTR) continue;
        close(tfd); return 1;                   /* blocks until expiry */
    }
    if (exp < 1) { close(tfd); return 1; }
    close(tfd);
    return 0;
}

/* 35. clock resolution + extra clocks ---------------------------------- */
int clock_res_test(void)
{
    static const clockid_t clocks[] = {
        CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW,
        CLOCK_BOOTTIME, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
    };
    struct timespec ts, res;
    unsigned i;
    int real_ok = 0, mono_ok = 0, extras = 0;
    for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        if (clock_getres(clocks[i], &res) != 0) continue;
        if (clock_gettime(clocks[i], &ts) != 0) continue;
        if (clocks[i] == CLOCK_REALTIME) real_ok = 1;
        else if (clocks[i] == CLOCK_MONOTONIC) mono_ok = 1;
        else extras++;
    }
    logts("  clocks: real=%d mono=%d extra=%d\n", real_ok, mono_ok, extras);
    if (!real_ok || !mono_ok) return 1;
    return 0;
}

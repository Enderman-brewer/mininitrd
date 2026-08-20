#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>

#include "init.h"

/* 10. CPU arithmetic + deterministic checksum ----------------------- */
static uint32_t cpu_burn(uint32_t iters)
{
    uint32_t x = 0x9e3779b9u, i;
    for (i = 0; i < iters; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x += 0x6d2b79f5u * (i & 0xffffu);
    }
    return x;
}

int cpu_calc_test(void)
{
    uint32_t a, b;
    uint32_t iters = 2000000u * (uint32_t)g_scale;
    if (g_scale == 1) iters = 500000u;
    a = cpu_burn(iters);
    b = cpu_burn(iters);
    if (a != b) return 1;               /* not deterministic -> broken */
    logts("  cpu burn checksum: %08x ok\n", a);
    return 0;
}

/* 11. pthreads ------------------------------------------------------ */
#define NTHREADS 4
static void *thread_work(void *arg)
{
    long id = (long)arg;
    uint64_t s = 0;
    int i;
    for (i = 0; i < 200000 * g_scale; i++)
        s += (uint64_t)(id + 1) * (uint64_t)i;
    return (void *)(intptr_t)s;
}
int cpu_threads_test(void)
{
    pthread_t t[NTHREADS];
    long i;
    for (i = 0; i < NTHREADS; i++) {
        if (pthread_create(&t[i], NULL, thread_work, (void *)i) != 0)
            return 1;
    }
    for (i = 0; i < NTHREADS; i++) {
        void *ret = NULL;
        if (pthread_join(t[i], &ret) != 0) return 1;
        if ((intptr_t)ret == 0) return 1;
    }
    return 0;
}

/* 12. scheduler ----------------------------------------------------- */
int sched_test(void)
{
    cpu_set_t set;
    int i, n = 0;
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return 1;
    for (i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &set)) n++;
    logts("  affinity: %d cpu(s)\n", n);
    if (n < 1) return 1;
    if (sched_yield() != 0) return 1;
    if (sched_get_priority_max(SCHED_OTHER) < 0) return 1;
    errno = 0;                          /* nice() only errors on -1 + errno */
    if (nice(0) < 0 && errno != 0) return 1;
    {
        struct sched_param sp;
        if (sched_getparam(0, &sp) != 0) return 1;
    }
    return 0;
}

/* 48. sched_getcpu / getcpu syscall -------------------------------------- */
int getcpu_test(void)
{
    unsigned cpu = 0, node = 0;
    long r = syscall(SYS_getcpu, &cpu, &node, NULL);
    if (r != 0) {
        if (errno == ENOSYS) return 2;
        return 1;
    }
    logts("  running on cpu %u (node %u)\n", cpu, node);
    if (sched_getcpu() < 0) return 1;
    return 0;
}

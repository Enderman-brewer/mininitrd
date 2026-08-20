#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include "init.h"

/* 7. malloc churn --------------------------------------------------- */
int mem_malloc_test(void)
{
    enum { N = 1200 };
    void *ptrs[N];
    size_t sizes[N];
    int i, n = N, got = 0;
    if (g_scale == 1) n = N / 4;
    for (i = 0; i < n; i++) {
        size_t sz = (size_t)(rng() % 262144) + 1;
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) { got = i; goto out_free; }
        sizes[i] = sz;
        memset(ptrs[i], (int)i, sz);
        got = i + 1;
    }
    for (i = 0; i < got; i++) {
        unsigned char *p = ptrs[i];
        size_t j;
        for (j = 0; j < sizes[i]; j++)
            if (p[j] != (unsigned char)i) { got = n; goto out_free; }
    }
    for (i = 0; i < got; i++)
        free(ptrs[i]);
    return 0;
out_free:
    for (i = 0; i < got; i++)
        free(ptrs[i]);
    return 1;
}

/* 8. mmap anon + touching pages ------------------------------------- */
int mem_mmap_test(void)
{
    size_t sz = 16 * 1024 * 1024;
    unsigned char *p;
    size_t i, stride;
    if (g_scale == 32) sz = 128 * 1024 * 1024;
    p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    stride = 4096;
    for (i = 0; i < sz; i += stride)
        p[i] = (unsigned char)(i >> 12);
    for (i = 0; i < sz; i += stride)
        if (p[i] != (unsigned char)(i >> 12)) { munmap(p, sz); return 1; }
    if (munmap(p, sz) != 0) return 1;
    /* mapped but never-touched region should not fault at unmap */
    p = mmap(NULL, 8 * 1024 * 1024, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    if (munmap(p, 8 * 1024 * 1024) != 0) return 1;
    return 0;
}

/* 9. mlock / munlock (SKIP if restricted) --------------------------- */
int mem_mlock_test(void)
{
    unsigned char *p = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    memset(p, 0x55, 1024 * 1024);
    if (mlock(p, 1024 * 1024) != 0) {
        munmap(p, 1024 * 1024);
        if (errno == EPERM || errno == ENOMEM) return 2;
        return 1;
    }
    if (munlock(p, 1024 * 1024) != 0) { munmap(p, 1024 * 1024); return 1; }
    if (munmap(p, 1024 * 1024) != 0) return 1;
    return 0;
}

/* 44. madvise ------------------------------------------------------------ */
int madvise_test(void)
{
    size_t sz = 1024 * 1024;
    unsigned char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    memset(p, 0x3c, sz);
    if (madvise(p, sz, MADV_DONTNEED) != 0) { munmap(p, sz); return 1; }
    if (madvise(p, sz, MADV_WILLNEED) != 0) { munmap(p, sz); return 1; }
    if (madvise(p, sz, MADV_RANDOM) != 0) { munmap(p, sz); return 1; }
    if (madvise(p, sz, MADV_NORMAL) != 0) { munmap(p, sz); return 1; }
    p[0] = 0x42;
    if (p[0] != 0x42) { munmap(p, sz); return 1; }
    munmap(p, sz);
    return 0;
}

/* 45. mlockall (SKIP if restricted) ------------------------------------- */
int mlockall_test(void)
{
    if (mlockall(MCL_CURRENT) != 0) {
        if (errno == EPERM || errno == ENOMEM || errno == EINVAL) return 2;
        return 1;
    }
    if (munlockall() != 0) return 1;
    return 0;
}

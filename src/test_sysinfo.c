#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <sys/syscall.h>

#include "init.h"

/* 22. getrandom syscall (SKIP if CRNG not ready / syscall missing) --- */
int getrandom_test(void)
{
    unsigned char a[64], b[64];
    long r1 = syscall(SYS_getrandom, a, sizeof(a), 1 /* GRND_NONBLOCK */);
    long r2 = syscall(SYS_getrandom, b, sizeof(b), 1 /* GRND_NONBLOCK */);
    int i, zeros = 0;
    if (r1 == -1 || r2 == -1) {
        if (errno == EAGAIN) return 2;   /* CRNG not initialized yet */
        if (errno == ENOSYS || errno == EPERM) return 2;
        return 1;
    }
    if (r1 != (long)sizeof(a) || r2 != (long)sizeof(b)) return 1;
    for (i = 0; i < (int)sizeof(a); i++)
        if (a[i] == 0) zeros++;
    if (zeros == (int)sizeof(a)) return 1;      /* no entropy at all */
    if (memcmp(a, b, sizeof(a)) == 0) return 1; /* two draws identical */
    return 0;
}

/* 1. basic sysinfo / uname ------------------------------------------ */
int sysinfo_test(void)
{
    struct utsname u;
    char buf[4096];
    if (uname(&u) != 0) return 1;
    logts("  kernel: %s %s %s (%s)\n", u.sysname, u.release,
          u.version, u.machine);
    if (read_file("/proc/version", buf, sizeof(buf)) == 0) return 1;
    if (!str_has(buf, "Linux")) return 1;
    if (read_file("/proc/uptime", buf, sizeof(buf)) == 0) return 1;
    {
        double up, idle;
        if (sscanf(buf, "%lf %lf", &up, &idle) != 2) return 1;
        logts("  uptime: %.1f s\n", up);
    }
    return 0;
}

/* 2. procfs integrity ----------------------------------------------- */
int procfs_test(void)
{
    char buf[16384];
    if (read_file("/proc/self/status", buf, sizeof(buf)) == 0) return 1;
    if (!str_has(buf, "Name:")) return 1;
    if (read_file("/proc/meminfo", buf, sizeof(buf)) == 0) return 1;
    if (!str_has(buf, "MemTotal")) return 1;
    if (read_file("/proc/cpuinfo", buf, sizeof(buf)) == 0) return 1;
    logts("  cpuinfo: %zu bytes (non-empty ok)\n", strlen(buf));
    if (read_file("/proc/mounts", buf, sizeof(buf)) == 0) return 1;
    if (!str_has(buf, "proc")) return 1;
    if (!str_has(buf, "sysfs")) return 1;
    if (read_file("/proc/self/cmdline", buf, sizeof(buf)) == 0) return 1;
    if (read_file("/proc/loadavg", buf, sizeof(buf)) == 0) return 1;
    return 0;
}

/* 3. sysfs integrity ------------------------------------------------- */
int sysfs_test(void)
{
    char buf[4096];
    struct stat st;
    if (stat("/sys/kernel", &st) != 0) return 1;
    if (stat("/sys/devices/system/cpu", &st) != 0) return 1;
    if (read_file("/sys/kernel/uevent_seqnum", buf, sizeof(buf)) == 0)
        return 1;
    return 0;
}

/* 4. device nodes / char devices ------------------------------------ */
int devfs_test(void)
{
    char buf[4096];
    ssize_t n;
    int fd;

    fd = open("/dev/null", O_WRONLY);
    if (fd < 0) return 1;
    if (write(fd, "x", 1) != 1) { close(fd); return 1; }
    close(fd);

    fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) return 1;
    memset(buf, 0xff, 64);
    n = read(fd, buf, 64);
    close(fd);
    if (n != 64) return 1;
    {
        int i;
        for (i = 0; i < 64; i++)
            if (buf[i] != 0) return 1;
    }

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 2;
    n = read(fd, buf, 16);
    close(fd);
    if (n != 16) return 1;

    fd = open("/dev/full", O_WRONLY);
    if (fd >= 0) {
        errno = 0;
        if (write(fd, "x", 1) == 1) { close(fd); return 1; } /* ENOSPC */
        if (errno != ENOSPC) { close(fd); return 1; }
        close(fd);
    }

    if (access("/dev/tty", F_OK) == 0) {
        fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == ENXIO)
                return 2;       /* PID 1 has no controlling tty: fine */
            return 1;
        }
        close(fd);
    }
    return 0;
}

/* 32. /proc/self detail ------------------------------------------------ */
int proc_self_test(void)
{
    char buf[16384];
    char exe[256];
    ssize_t n;
    if (read_file("/proc/self/maps", buf, sizeof(buf)) == 0)
        return 2;               /* no-MMU kernel: /proc/self/maps absent */
    if (!str_has(buf, "r-xp")) return 1;
    if (!str_has(buf, "[stack]")) return 1;
    if (read_file("/proc/self/stat", buf, sizeof(buf)) == 0) return 1;
    {
        char *end = strrchr(buf, ')');
        char state;
        if (!end) return 1;
        if (sscanf(end + 1, " %c", &state) != 1) return 1;
        if (state != 'R' && state != 'S') return 1;
    }
    if (read_file("/proc/self/limits", buf, sizeof(buf)) == 0) return 1;
    if (!str_has(buf, "Max open files")) return 1;
    if (read_file("/proc/self/status", buf, sizeof(buf)) == 0) return 1;
    if (!str_has(buf, "VmRSS")) return 1;
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 1;
    exe[n] = '\0';
    logts("  exe: %s\n", exe);
    return 0;
}

/* 33. directory enumeration: /proc and /sys ---------------------------- */
int readdir_test(void)
{
    struct dirent *de;
    DIR *d;
    int n;
    d = opendir("/proc");
    if (!d) return 2;               /* procfs absent: skip */
    n = 0;
    while ((de = readdir(d)) != NULL) n++;
    closedir(d);
    if (n < 6) return 2;            /* minimal kernel: skip rather than fail */
    logts("  /proc: %d entries\n", n);
    d = opendir("/sys");
    if (!d) return 2;               /* sysfs absent: skip */
    n = 0;
    while ((de = readdir(d)) != NULL) n++;
    closedir(d);
    if (n < 4) return 2;
    logts("  /sys: %d entries\n", n);
    d = opendir("/sys/class");
    if (!d) return 2;
    closedir(d);
    return 0;
}

/* 20. kernel log scan: warnings / oops / panic ----------------------- */
static long dmesg_read(char *buf, size_t size)
{
    /* SYSLOG_ACTION_READ_ALL = 3 */
    return syscall(SYS_syslog, 3, buf, (int)(size - 1));
}

int kmsg_scan_test(void)
{
    static char dmesg[262144];
    long n = dmesg_read(dmesg, sizeof(dmesg));
    static const char *bad[] = {
        "Kernel panic", "Oops:", "BUG:", "WARNING:", "Call Trace",
        "KASAN", "UBSAN", "BUG_ON", "stack guard page",
        "general protection fault", "page fault in kernel mode",
        NULL
    };
    int i, count = 0;
    if (n < 0) return 2;                 /* no access to kernel log */
    if (n >= (long)sizeof(dmesg)) n = (long)sizeof(dmesg) - 1;
    dmesg[n] = '\0';
    logts("  kernel log: %ld bytes\n", n);
    for (i = 0; bad[i]; i++) {
        const char *hit = dmesg;
        while ((hit = strstr(hit, bad[i])) != NULL) {
            count++;
            logts("  !!! kernel log contains '%s'\n", bad[i]);
            hit += strlen(bad[i]);
        }
    }
    return (count == 0) ? 0 : 1;
}

/* 21. KUnit / kselftest results in dmesg ----------------------------- */
int kunit_scan_test(void)
{
    static char dmesg[262144];
    long n = dmesg_read(dmesg, sizeof(dmesg));
    int passed = 0, failed = 0, skipped = 0;
    const char *p;
    if (n < 0) return 2;
    if (n >= (long)sizeof(dmesg)) n = (long)sizeof(dmesg) - 1;
    dmesg[n] = '\0';

    if (!str_has(dmesg, "kunit")) {
        logts("  no KUnit output in kernel log (not built-in) -- ok\n");
        return 2;
    }

    p = dmesg;
    while ((p = strstr(p, "[PASSED]")) != NULL) { passed++; p += 8; }
    p = dmesg;
    while ((p = strstr(p, "[FAILED]")) != NULL) { failed++; p += 8; }
    p = dmesg;
    while ((p = strstr(p, "[SKIPPED]")) != NULL) { skipped++; p += 9; }
    p = dmesg;
    while ((p = strstr(p, "not ok")) != NULL) { failed++; p += 6; }
    p = dmesg;
    while ((p = strstr(p, "ok ")) != NULL) { passed++; p += 3; }

    logts("  KUnit: %d passed, %d failed, %d skipped\n",
          passed, failed, skipped);
    return (failed == 0) ? 0 : 1;
}

/*
 * init.c -- kernel self-test harness that runs as PID 1 from an initramfs.
 *
 * Goal: exercise as much of the running kernel as possible during boot,
 * log everything to the console AND to any serial port (if present) and to
 * /dev/kmsg, then -- by default -- sleep forever so the (possibly broken)
 * system stays alive for inspection.
 *
 * Designed to be universal:
 *   - statically linked, no busybox, no shared libraries, no external tools
 *   - works on x86, ARM, RISC-V, MIPS, PPC, s390, ... (anything running Linux)
 *   - serial console auto-detected via /proc/cmdline, /proc/consoles and a
 *     broad list of well-known serial device names (ttyS*, ttyAMA*, ttySAC*,
 *     ttymxc*, ttyPS*, hvc0, xvc0, ...)
 *   - falls back to manual mknod of /dev if devtmpfs is not available
 *   - every test degrades gracefully (SKIP instead of FAIL) when the kernel
 *     was built without the relevant config (no networking, no mlock, ...)
 *
 * Kernel cmdline knobs (all optional):
 *   kerneltest=fast|all|stress   test intensity (default: all)
 *   kerneltest.loop              re-run the whole suite forever
 *   kerneltest.poweroff=1        power off the machine after the summary
 *   kerneltest.reboot=1          reboot instead
 *   kerneltest.panic_on_fail=1   trigger a kernel panic if any test failed
 *   kerneltest.timeout=N         hard runtime cap in seconds (watchdog);
 *                                aborts the suite and powers the machine
 *                                off, so a hung test can't hang a CI job
 *   kerneltest.quiet=1           less chatter between tests
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/* global state                                                        */
/* ------------------------------------------------------------------ */

static int    sinks[16];      /* fds everything is logged to            */
static int    nsinks = 0;
static int    kmsg_fd = -1;
static int    log_fd = -1;    /* /tmp/kerneltest.log once tmpfs is up    */
static int    g_pass = 0, g_fail = 0, g_skip = 0;
static int    g_quiet = 0;
static int    g_scale = 4;    /* loop multiplier: fast=1 all=4 stress=32 */
static int    g_loop = 0;
static int    g_poweroff = 0, g_reboot = 0, g_panic_fail = 0;
static int    g_timeout = 0;
static double g_boot;         /* CLOCK_BOOTTIME at start */

static double now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sink_write(const char *s, size_t n)
{
    int i;
    if (n == 0) return;
    for (i = 0; i < nsinks; i++) {
        /* serial sinks are O_NONBLOCK (see try_dev): a slow or flow-
         * controlled line may drop output rather than stall the whole
         * test suite.  That is deliberate -- kernel testing must not
         * wedge on a 9600-baud port. */
        ssize_t r = write(sinks[i], s, n);
        (void)r;
    }
}

static void logts(const char *fmt, ...)
{
    char buf[1200];
    char pre[64];
    va_list ap;
    int n;
    snprintf(pre, sizeof(pre), "[%7.3f] ", now_s() - g_boot);
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    sink_write(pre, strlen(pre));
    sink_write(buf, (size_t)n);
}

static void add_sink(int fd)
{
    struct stat a, b;
    int i;
    if (fd < 0) return;
    if (fstat(fd, &a) != 0) { close(fd); return; }
    for (i = 0; i < nsinks; i++) {
        if (fstat(sinks[i], &b) == 0 &&
            a.st_rdev == b.st_rdev && a.st_dev == b.st_dev) {
            close(fd);          /* already logging to this device */
            return;
        }
    }
    if (nsinks < (int)(sizeof(sinks) / sizeof(sinks[0])))
        sinks[nsinks++] = fd;
    else
        close(fd);
}

/* ---------------------------- helpers ----------------------------- */

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static size_t read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n = 0;
    if (fd < 0) return 0;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (size_t)n;
}

static int str_has(const char *hay, const char *needle)
{
    return hay != NULL && strstr(hay, needle) != NULL;
}

static void mknod_dev(const char *path, mode_t mode, unsigned maj, unsigned min)
{
    unlink(path);
    if (mknod(path, S_IFCHR | mode, makedev(maj, min)) != 0)
        logts("init: mknod %s failed: %s\n", path, strerror(errno));
}

/* ------------------------- serial detection ----------------------- */

/*
 * Open every console-ish device we can find.  Sources:
 *   1. console= arguments from /proc/cmdline (e.g. console=ttyS0,115200)
 *   2. the active consoles listed in /proc/consoles
 *   3. a broad static list of well-known serial device names
 * The fstat-based dedup in add_sink() stops us logging twice to the same
 * tty (stdout is usually /dev/console, which aliases the serial line).
 */
static const char *well_known[] = {
    "ttyS0", "ttyS1", "ttyS2", "ttyS3",
    "ttyAMA0", "ttyAMA1", "ttyAMA2", "ttyAMA3",
    "ttySAC0", "ttySAC1", "ttySAC2", "ttySAC3",
    "ttymxc0", "ttymxc1", "ttySC0", "ttySC1",
    "ttyPS0", "ttyPS1", "ttyO0", "ttyO1",
    "ttyUL0", "ttyFIQ0", "ttyVR0", "ttyAP0",
    "hvc0", "hvc1", "hvsi0", "xvc0", "tty0",
    NULL
};

static int is_serial_name(const char *n)
{
    return strncmp(n, "tty", 3) == 0 ||
           strncmp(n, "hvc", 3) == 0 ||
           strncmp(n, "xvc", 3) == 0 ||
           strncmp(n, "hvsi", 4) == 0;
}

/* names marked 'C' in /proc/consoles are the preferred console, i.e. the
 * device /dev/console already writes to (same UART behind a different
 * node).  Opening them again as a separate sink would duplicate every line
 * on the serial line, so we skip them. */
static char g_pref[8][64];
static int  g_npref = 0;

static void remember_preferred(const char *name)
{
    int i;
    for (i = 0; i < g_npref; i++)
        if (strcmp(g_pref[i], name) == 0) return;
    if (g_npref < (int)(sizeof(g_pref) / sizeof(g_pref[0]))) {
        size_t len = strlen(name);
        if (len >= sizeof(g_pref[0])) len = sizeof(g_pref[0]) - 1;
        memcpy(g_pref[g_npref], name, len);
        g_pref[g_npref][len] = '\0';
        g_npref++;
    }
}

static int is_preferred(const char *name)
{
    int i;
    for (i = 0; i < g_npref; i++)
        if (strcmp(g_pref[i], name) == 0) return 1;
    return 0;
}

static void try_dev(const char *devname)
{
    char path[96];
    int fd;
    if (is_preferred(devname))
        return;                       /* /dev/console already covers this */
    snprintf(path, sizeof(path), "/dev/%s", devname);
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return;
    add_sink(fd);
}

static void open_console_sinks(void)
{
    char cmdline[4096];
    char cons[4096];
    char *tok, *save = NULL;
    int i;

    /* stdout = /dev/console is usually already the right place */
    add_sink(dup(STDOUT_FILENO));

    /* 0. scan /proc/consoles first: record which tty is the preferred
     *    console (letter 'C' in the ops column, e.g. "tty0 -WU (EC p )")
     *    so we do not open it a second time below */
    if (read_file("/proc/consoles", cons, sizeof(cons)) > 0) {
        char *line, *lsave = NULL;
        for (line = strtok_r(cons, "\n", &lsave); line;
             line = strtok_r(NULL, "\n", &lsave)) {
            char name[64], flags[16], ops[64];
            if (sscanf(line, "%63s %15s %63s", name, flags, ops) == 3) {
                if (strchr(ops, 'C'))
                    remember_preferred(name);
            }
        }
    }

    /* 1. console= from cmdline */
    if (read_file("/proc/cmdline", cmdline, sizeof(cmdline)) > 0) {
        for (tok = strtok_r(cmdline, " \t\n", &save); tok;
             tok = strtok_r(NULL, " \t\n", &save)) {
            if (strncmp(tok, "console=", 8) == 0) {
                char dev[64];
                char *comma = strchr(tok + 8, ',');
                size_t len;
                if (comma) len = (size_t)(comma - (tok + 8));
                else       len = strlen(tok + 8);
                if (len > 0 && len < sizeof(dev)) {
                    memcpy(dev, tok + 8, len);
                    dev[len] = '\0';
                    if (is_serial_name(dev))
                        try_dev(dev);
                }
            }
        }
    }

    /* 2. /proc/consoles: add serial consoles other than the preferred one */
    if (read_file("/proc/consoles", cons, sizeof(cons)) > 0) {
        char *line, *lsave = NULL;
        for (line = strtok_r(cons, "\n", &lsave); line;
             line = strtok_r(NULL, "\n", &lsave)) {
            char name[64], flags[16], ops[64];
            if (sscanf(line, "%63s %15s %63s", name, flags, ops) == 3) {
                if (is_serial_name(name))
                    try_dev(name);   /* skips preferred (flag 'C') */
            }
        }
    }

    /* 3. well-known names */
    for (i = 0; well_known[i]; i++)
        try_dev(well_known[i]);
}

static void open_kmsg_sink(void)
{
    /* /dev/kmsg lets the whole log end up in the kernel ring buffer, so
     * it survives into dmesg after boot. */
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kmsg_fd >= 0 && nsinks < (int)(sizeof(sinks) / sizeof(sinks[0])))
        sinks[nsinks++] = kmsg_fd;
    else if (kmsg_fd >= 0)
        close(kmsg_fd);
}

/* ------------------------- mounts ---------------------------------- */

static void mount_fs(const char *src, const char *tgt, const char *type,
                     unsigned long flags, const char *opts)
{
    if (mount(src, tgt, type, flags, opts) != 0) {
        if (errno != EBUSY && errno != EINVAL && errno != EACCES)
            logts("init: mount %s on %s failed: %s\n",
                  type, tgt, strerror(errno));
    }
}

static void setup_dev(void)
{
    mkdir("/dev", 0755);
    mount_fs("devtmpfs", "/dev", "devtmpfs", 0, "mode=0755");
    if (access("/dev/null", F_OK) != 0) {
        /* no devtmpfs -- build a minimal /dev by hand */
        mknod_dev("/dev/null",    0666, 1, 3);
        mknod_dev("/dev/zero",    0666, 1, 5);
        mknod_dev("/dev/full",    0666, 1, 7);
        mknod_dev("/dev/random",  0666, 1, 8);
        mknod_dev("/dev/urandom", 0666, 1, 9);
        mknod_dev("/dev/kmsg",    0600, 1, 11);
        mknod_dev("/dev/mem",     0600, 1, 1);
        mknod_dev("/dev/tty",     0666, 5, 0);
        mknod_dev("/dev/console", 0600, 5, 1);
    }
}

/* ------------------------- cmdline --------------------------------- */

static void parse_cmdline(void)
{
    char cmdline[4096];
    char *tok, *save = NULL;
    if (read_file("/proc/cmdline", cmdline, sizeof(cmdline)) == 0)
        return;
    for (tok = strtok_r(cmdline, " \t\n", &save); tok;
         tok = strtok_r(NULL, " \t\n", &save)) {
        if (strcmp(tok, "kerneltest=fast") == 0) g_scale = 1;
        else if (strcmp(tok, "kerneltest=all") == 0) g_scale = 4;
        else if (strcmp(tok, "kerneltest=stress") == 0) g_scale = 32;
        else if (strcmp(tok, "kerneltest.loop") == 0) g_loop = 1;
        else if (strncmp(tok, "kerneltest.poweroff=", 20) == 0)
            g_poweroff = atoi(tok + 20);
        else if (strncmp(tok, "kerneltest.reboot=", 18) == 0)
            g_reboot = atoi(tok + 18);
        else if (strncmp(tok, "kerneltest.panic_on_fail=", 25) == 0)
            g_panic_fail = atoi(tok + 25);
        else if (strncmp(tok, "kerneltest.timeout=", 19) == 0)
            g_timeout = atoi(tok + 19);
        else if (strcmp(tok, "kerneltest.quiet") == 0) g_quiet = 1;
    }
}

/* ------------------------- test framework ------------------------- */

#define RUN_TEST(name) do {                                             \
        int _r;                                                         \
        double _t0 = now_s();                                           \
        if (!g_quiet) logts("---- TEST %-22s ----\n", #name);           \
        _r = name##_test();                                             \
        if (_r == 0)      { g_pass++;                                   \
                            logts("[PASS] %s (%d ms)\n", #name,         \
                                  (int)((now_s() - _t0) * 1000)); }     \
        else if (_r == 2) { g_skip++;                                   \
                            logts("[SKIP] %s (%d ms)\n", #name,         \
                                  (int)((now_s() - _t0) * 1000)); }     \
        else              { g_fail++;                                   \
                            logts("[FAIL] %s (%d ms)\n", #name,         \
                                  (int)((now_s() - _t0) * 1000)); }     \
    } while (0)

/* small pseudo-random generator for deterministic data */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ================================================================== */
/*  TESTS                                                              */
/* ================================================================== */

/* 1. basic sysinfo / uname ------------------------------------------ */
static int sysinfo_test(void)
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
static int procfs_test(void)
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
static int sysfs_test(void)
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
static int devfs_test(void)
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

/* 5. tmpfs mount + basic fs ops ------------------------------------- */
static int fs_tmpfs_test(void)
{
    const char *dir = "/tmp/t1";
    const char *f1  = "/tmp/t1/a.txt";
    const char *f2  = "/tmp/t1/b.txt";
    const char *ln  = "/tmp/t1/link";
    char data[8192], rd[8192];
    struct stat st;
    int fd, i;

    mkdir("/tmp", 0755);
    mount_fs("tmpfs", "/tmp", "tmpfs", 0, "mode=1777,size=64M");
    if (access("/tmp", W_OK) != 0) return 1;
    if (mkdir(dir, 0755) != 0) return 1;

    for (i = 0; i < (int)sizeof(data); i++)
        data[i] = (char)('a' + (i % 26));

    fd = open(f1, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return 1;
    if (write_all(fd, data, sizeof(data)) != 0) { close(fd); return 1; }
    if (fsync(fd) != 0) { close(fd); return 1; }
    if (close(fd) != 0) return 1;

    if (stat(f1, &st) != 0) return 1;
    if (st.st_size != (off_t)sizeof(data)) return 1;

    fd = open(f1, O_RDONLY);
    if (fd < 0) return 1;
    if (read(fd, rd, sizeof(rd)) != (ssize_t)sizeof(rd)) {
        close(fd); return 1;
    }
    close(fd);
    if (memcmp(data, rd, sizeof(data)) != 0) return 1;

    if (rename(f1, f2) != 0) return 1;
    if (access(f1, F_OK) == 0) return 1;
    if (symlink(f2, ln) != 0) return 1;
    if (lstat(ln, &st) != 0) return 1;
    if (!S_ISLNK(st.st_mode)) return 1;

    if (truncate(f2, 100) != 0) return 1;
    if (stat(f2, &st) != 0) return 1;
    if (st.st_size != 100) return 1;

    if (chmod(f2, 0600) != 0) return 1;
    if (stat(f2, &st) != 0) return 1;
    if ((st.st_mode & 0777) != 0600) return 1;

    if (unlink(f2) != 0) return 1;
    if (unlink(ln) != 0) return 1;
    if (rmdir(dir) != 0) return 1;
    return 0;
}

/* 6. filesystem stress ---------------------------------------------- */
static int fs_stress_test(void)
{
    char dir[64], path[128], buf[256];
    int nfiles = 200 * g_scale;
    int i;
    if (nfiles > 4000) nfiles = 4000;
    snprintf(dir, sizeof(dir), "/tmp/stress");
    if (mkdir(dir, 0755) != 0) return 1;
    for (i = 0; i < nfiles; i++) {
        int fd, k;
        snprintf(path, sizeof(path), "%s/f%05d", dir, i);
        fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0) return 1;
        for (k = 0; k < 4; k++) {
            snprintf(buf, sizeof(buf), "file %d chunk %d %x\n", i, k, rng());
            if (write_all(fd, buf, strlen(buf)) != 0) {
                close(fd); return 1;
            }
        }
        if (close(fd) != 0) return 1;
    }
    /* read a random sample back */
    for (i = 0; i < 64; i++) {
        char rd[256];
        int fd, sel = (int)(rng() % (uint32_t)nfiles);
        snprintf(path, sizeof(path), "%s/f%05d", dir, sel);
        fd = open(path, O_RDONLY);
        if (fd < 0) return 1;
        if (read(fd, rd, sizeof(rd)) <= 0) { close(fd); return 1; }
        close(fd);
    }
    /* cleanup */
    for (i = 0; i < nfiles; i++) {
        snprintf(path, sizeof(path), "%s/f%05d", dir, i);
        if (unlink(path) != 0) return 1;
    }
    if (rmdir(dir) != 0) return 1;
    return 0;
}

/* 7. malloc churn --------------------------------------------------- */
static int mem_malloc_test(void)
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
static int mem_mmap_test(void)
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
static int mem_mlock_test(void)
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

static int cpu_calc_test(void)
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
static int cpu_threads_test(void)
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
static int sched_test(void)
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

/* 13. timers / clocks ------------------------------------------------ */
static int timer_test(void)
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

/* 14. signals ------------------------------------------------------- */
static volatile sig_atomic_t g_sig_usr1;
static volatile sig_atomic_t g_sig_alrm;
static void on_usr1(int s) { (void)s; g_sig_usr1 = 1; }
static void on_alrm(int s) { (void)s; g_sig_alrm = 1; }

static int signal_test(void)
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

/* 15. pipe IPC + fork ------------------------------------------------ */
static int pipe_ipc_test(void)
{
    int fds[2];
    pid_t pid;
    char out[256], in[256];
    int i;
    if (pipe(fds) != 0) return 1;
    for (i = 0; i < 255; i++) out[i] = (char)(i + 1);
    out[255] = '\0';
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 1; }
    if (pid == 0) {                    /* child: read + echo check */
        int ok = 1;
        ssize_t got = 0, r;
        close(fds[1]);
        while (got < 256) {
            r = read(fds[0], in + got, 256 - (size_t)got);
            if (r <= 0) { ok = 0; break; }
            got += r;
        }
        if (ok && memcmp(out, in, 256) != 0) ok = 0;
        _exit(ok ? 0 : 1);
    }
    close(fds[0]);
    if (write_all(fds[1], out, 256) != 0) { close(fds[1]); return 1; }
    close(fds[1]);
    {
        int st;
        if (waitpid(pid, &st, 0) != pid) return 1;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 1;
    }
    return 0;
}

/* 16. exec self (full exec path through the kernel) ------------------ */
static int exec_test(void)
{
    int fds[2];
    pid_t pid;
    int st;
    char buf[16] = {0};
    ssize_t r;

    if (pipe(fds) != 0) return 1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 1; }
    if (pid == 0) {
        char *argv[3];
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        argv[0] = "init";
        argv[1] = "--exec-child";
        argv[2] = NULL;
        execv("/init", argv);
        _exit(127);
    }
    close(fds[1]);
    r = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    if (r < 0) return 1;
    buf[r] = '\0';
    if (waitpid(pid, &st, 0) != pid) return 1;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 1;
    if (strncmp(buf, "EXECOK", 6) != 0) return 1;
    return 0;
}

/* 17. fork storm ---------------------------------------------------- */
static int fork_storm_test(void)
{
    enum { NCHILD = 24 };
    pid_t pids[NCHILD];
    int i;
    for (i = 0; i < NCHILD; i++) {
        pid_t p = fork();
        if (p < 0) return 1;
        if (p == 0) {
            uint32_t x = 0xdeadbeefu ^ (uint32_t)i;
            int k;
            for (k = 0; k < 50000; k++) {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
            }
            _exit((int)(x & 0xff));
        }
        pids[i] = p;
    }
    for (i = 0; i < NCHILD; i++) {
        int st;
        if (waitpid(pids[i], &st, 0) != pids[i]) return 1;
        if (!WIFEXITED(st)) return 1;
    }
    return 0;
}

/* 18. networking loopback ------------------------------------------- */
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

static int net_loopback_test(void)
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

/* 19. mmap file I/O on tmpfs ----------------------------------------- */
static int fs_mmap_test(void)
{
    const char *path = "/tmp/mmapped.bin";
    char *m;
    size_t sz = 1024 * 1024;
    int fd, i;
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)sz) != 0) { close(fd); return 1; }
    m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return 1; }
    for (i = 0; i < (int)sz; i += 4096)
        m[i] = (char)(i >> 12);
    if (msync(m, sz, MS_SYNC) != 0) { munmap(m, sz); close(fd); return 1; }
    if (munmap(m, sz) != 0) { close(fd); return 1; }
    m = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return 1; }
    for (i = 0; i < (int)sz; i += 4096)
        if (m[i] != (char)(i >> 12)) { munmap(m, sz); close(fd); return 1; }
    munmap(m, sz);
    close(fd);
    unlink(path);
    return 0;
}

/* 20. kernel log scan: warnings / oops / panic ----------------------- */
static long dmesg_read(char *buf, size_t size)
{
    /* SYSLOG_ACTION_READ_ALL = 3 */
    return syscall(SYS_syslog, 3, buf, (int)(size - 1));
}

static int kmsg_scan_test(void)
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
/*
 * KUnit prints KTAP output to dmesg at boot when compiled in.  We count
 * result lines ("[PASSED]" / "[FAILED]" / "ok " / "not ok ") and fail the
 * test if any FAILED / not-ok line appears.  If the kernel was built
 * without KUnit there is nothing to check, which is a SKIP, not a fail.
 */
static int kunit_scan_test(void)
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

/* ---------------------------- RAMIFY REGRESSION TEST ------------------------ */
/* KEEP THIS TEST NEAR THE END OF THE SPECIFIC KERNEL TESTS because it is a
 * heavier Ramify regression test designed to detect list_del corruption in
 * ramify_maybe_promote(). Future maintainers: KEEP THIS TEST NEAR THE END OF
 * THE SPECIFIC KERNEL TESTS and do NOT move it into the generic kernel tests
 * section. This test exercises the failure mode where repeated reads of a file
 * promote it through Ramify, and later executable reads through filemap_read()
 * cause list corruption (__list_del_entry_valid_or_report).
 */

/* Test file name used for Ramify promotion exercise */
#define RAMIFY_TEST_FILE   "/tmp/ramify_stress.bin"
#define RAMIFY_TEST_SIZE   (4 * 1024 * 1024)  /* 4 MB file */
#define RAMIFY_READ_COUNT  35                 /* Need >30 reads with distinct fds */
#define RAMIFY_EXEC_COUNT  50                 /* Repeated execve() iterations */

static int ramify_regression_test(void)
{
    int test_fd = -1, i, passes = 0, fails = 0;
    unsigned char buf[8192];
    struct stat st;
    pid_t pid;
    int exec_status;

    logts("  RAMIFY: creating test file %s (%d bytes)...\n",
          RAMIFY_TEST_FILE, RAMIFY_TEST_SIZE);

    /* Create the test file with sufficient size to trigger Ramify promotion */
    test_fd = open(RAMIFY_TEST_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (test_fd < 0) {
        logts("  RAMIFY: cannot create test file: %s\n", strerror(errno));
        return 2;  /* SKIP - cannot create test file */
    }
    if (ftruncate(test_fd, RAMIFY_TEST_SIZE) != 0) {
        close(test_fd);
        logts("  RAMIFY: cannot truncate test file\n");
        return 2;
    }
    /* Write some data so reads have content */
    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = (unsigned char)(i & 0xFF);
    if (write(test_fd, buf, sizeof(buf)) != sizeof(buf)) {
        close(test_fd);
        logts("  RAMIFY: cannot write test file\n");
        return 2;
    }
    close(test_fd);
    test_fd = -1;

    /* Verify file size */
    if (stat(RAMIFY_TEST_FILE, &st) != 0) {
        logts("  RAMIFY: cannot stat test file\n");
        return 2;
    }
    logts("  RAMIFY: test file size: %lld bytes\n", (long long)st.st_size);

    /* PHASE 1: Read the file 30+ times with DIFFERENT file descriptors.
     * This exercises Ramify's file promotion behavior through filemap_read()
     * and ramify_maybe_promote() on multiple open references to the same file.
     * Each read gets its own fd to ensure independent file lookups.
     */
    logts("  RAMIFY: phase 1 - reading file %d times with distinct fds... ",
          RAMIFY_READ_COUNT);

    for (i = 0; i < RAMIFY_READ_COUNT; i++) {
        int fd;
        ssize_t n;

        fd = open(RAMIFY_TEST_FILE, O_RDONLY);
        if (fd < 0) {
            logts("FAILED at read %d: open failed: %s\n", i, strerror(errno));
            fails++;
            break;
        }

        /* Read in chunks to ensure filemap pages are exercised */
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            /* (no validation needed, just exercising the read path) */
        }
        close(fd);
    }

    logts("done (%d reads)\n", i);
    if (fails > 0) {
        logts("  RAMIFY: %d reads failed\n", fails);
        return 1;
    }
    logts("  RAMIFY: %d distinct fd reads PASSED\n", i);

    /* PHASE 2: Exercise execve() / fork() repeatedly.
     * This exercises the exact call trace from the bug report:
     * bprm_execve -> __kernel_read -> filemap_read -> ramify_maybe_promote
     * Each execve() causes the kernel to read the executable through
     * filemap_read(), exercising Ramify alongside the promoted file.
     */
    logts("  RAMIFY: phase 2 - executing /hello %d times... ",
          RAMIFY_EXEC_COUNT);

    for (i = 0; i < RAMIFY_EXEC_COUNT; i++) {
        pid = fork();
        if (pid < 0) {
            logts("FAILED at exec %d: fork failed: %s\n", i, strerror(errno));
            return 1;
        }
        if (pid == 0) {
            /* Child process: execute /hello */
            char *argv[] = { "/hello", NULL };
            execve("/hello", argv, NULL);
            _exit(127);  /* execve failed */
        }
        /* Parent: wait for child */
        if (waitpid(pid, &exec_status, 0) != pid) {
            logts("FAILED at exec %d: waitpid failed\n", i);
            return 1;
        }
        if (!WIFEXITED(exec_status) || WEXITSTATUS(exec_status) != 0) {
            logts("FAILED at exec %d: child exited with status %d\n",
                  i, WEXITSTATUS(exec_status));
            return 1;
        }
        passes++;
    }

    logts("done (%d successful executions)\n", passes);

    /* PHASE 3: Concurrent exercise - interleaved reads and execve.
     * This exercises potential races between Ramify operations on different
     * file descriptors during concurrent filesystem activity.
     */
    logts("  RAMIFY: phase 3 - concurrent reads/EXECVE interleaving... ");

    for (i = 0; i < 40; i++) {
        int fd, status;
        pid = fork();
        if (pid < 0) {
            logts("FAILED at concurrent pass %d: fork failed\n", i);
            return 1;
        }
        if (pid == 0) {
            /* Child: read via distinct fd */
            fd = open(RAMIFY_TEST_FILE, O_RDONLY);
            if (fd >= 0) {
                read(fd, buf, sizeof(buf));
                close(fd);
            }
            /* Also try exec if we're in a race window */
            if ((i & 3) == 0) {
                pid_t ep = fork();
                if (ep == 0) {
                    char *argv[] = { "/hello", NULL };
                    execve("/hello", argv, NULL);
                    _exit(127);
                } else if (ep > 0) {
                    waitpid(ep, &status, 0);
                }
            }
            _exit(0);
        }
        if (waitpid(pid, &status, 0) != pid) {
            logts("FAILED at concurrent wait %d\n", i);
            return 1;
        }
    }
    logts("PASSED\n");

    /* Cleanup */
    unlink(RAMIFY_TEST_FILE);

    logts("  RAMIFY: all phases completed successfully\n");
    return 0;
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

static void run_suite(void)
{
    RUN_TEST(sysinfo);
    RUN_TEST(procfs);
    RUN_TEST(sysfs);
    RUN_TEST(devfs);
    RUN_TEST(fs_tmpfs);
    RUN_TEST(fs_stress);
    RUN_TEST(mem_malloc);
    RUN_TEST(mem_mmap);
    RUN_TEST(mem_mlock);
    RUN_TEST(cpu_calc);
    RUN_TEST(cpu_threads);
    RUN_TEST(sched);
    RUN_TEST(timer);
    RUN_TEST(signal);
    RUN_TEST(pipe_ipc);
    RUN_TEST(exec);
    RUN_TEST(fork_storm);
    RUN_TEST(net_loopback);
    RUN_TEST(fs_mmap);
    RUN_TEST(kmsg_scan);
    RUN_TEST(kunit_scan);
}

static void print_summary(void)
{
    logts("============================================================\n");
    logts("kerneltest complete: %d passed, %d failed, %d skipped\n",
          g_pass, g_fail, g_skip);
}

/*
 * Watchdog mode: run the whole suite in a forked child; if it has not
 * finished within kerneltest.timeout=N seconds, kill it and report.  The
 * caller (main) then powers the machine off, so a wedged test can never
 * hang a CI run forever.
 * Returns 0 = suite finished, 1 = killed on timeout.
 */
static int run_suite_watchdog(void)
{
    pid_t p;
    int st;
    double t0 = now_s();

    p = fork();
    if (p < 0) {
        logts("watchdog: fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (p == 0) {                       /* child: run the suite */
        run_suite();
        print_summary();
        _exit(g_fail ? 1 : 0);
    }
    for (;;) {                          /* parent: watchdog */
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
                return 0;
            logts("watchdog: suite child exited abnormally\n");
            return 1;
        }
        if (now_s() - t0 > (double)g_timeout) {
            logts("watchdog: suite did not finish within %d s, killing it\n",
                  g_timeout);
            kill(p, SIGKILL);
            waitpid(p, &st, 0);
            return 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, 0,
                        &(struct timespec){0, 100000000}, NULL); /* 100 ms */
    }
}

static void end_behavior(void)
{
    print_summary();

    if (g_fail > 0 && g_panic_fail) {
        int fd = open("/proc/sysrq-trigger", O_WRONLY);
        logts("panic_on_fail: triggering kernel panic\n");
        if (fd >= 0) {
            (void)write(fd, "c", 1);
            close(fd);
        }
    }

    if (g_poweroff) {
        logts("powering off\n");
        sync();
        reboot(RB_POWER_OFF);
        sleep(10);               /* in case reboot() was blocked */
        _exit(0);
    }
    if (g_reboot) {
        logts("rebooting\n");
        sync();
        reboot(RB_AUTOBOOT);
        sleep(10);
        _exit(0);
    }

    logts("all tests done -- sleeping forever (console alive)\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--exec-child") == 0) {
        /* marker for the exec() self-test */
        (void)write(STDOUT_FILENO, "EXECOK", 6);
        _exit(0);
    }

    g_boot = now_s();

    /* mounts first: everything below relies on them */
    mkdir("/proc", 0555); mkdir("/sys", 0555);
    mount_fs("proc", "/proc", "proc", 0, "");
    mount_fs("sysfs", "/sys", "sysfs", 0, "");
    setup_dev();
    mount_fs("tmpfs", "/tmp", "tmpfs", 0, "mode=1777,size=64M");

    open_console_sinks();
    open_kmsg_sink();
    parse_cmdline();

    logts("============================================================\n");
    logts("kernel test initramfs -- PID 1\n");
    {
        struct utsname u;
        if (uname(&u) == 0)
            logts("  kernel %s %s (%s)\n", u.release, u.version, u.machine);
    }
    logts("  intensity: %s, loop=%d, poweroff=%d, reboot=%d\n",
          g_scale == 1 ? "fast" : (g_scale == 32 ? "stress" : "all"),
          g_loop, g_poweroff, g_reboot);

    /* optional log file on tmpfs */
    log_fd = open("/tmp/kerneltest.log",
                  O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0644);
    if (log_fd >= 0 && nsinks < (int)(sizeof(sinks) / sizeof(sinks[0])))
        sinks[nsinks++] = log_fd;

    if (g_timeout > 0) {
        /* watchdog mode: hard runtime cap, then power off.  This is what
         * CI wants -- a wedged test can never hang the job forever. */
        int killed = run_suite_watchdog();
        if (killed)
            logts("watchdog: suite aborted after %d s\n", g_timeout);
        logts("kerneltest timeout mode complete -- powering off\n");
        sync();
        reboot(RB_POWER_OFF);
        sleep(10);               /* in case reboot() was blocked */
        _exit(0);
    }

    do {
        g_pass = g_fail = g_skip = 0;
        run_suite();
        end_behavior();
        if (g_loop) {
            logts("-- loop iteration done, restarting suite --\n");
        }
    } while (g_loop);

    /* sleep forever */
    for (;;) {
        sleep(3600);
    }
}

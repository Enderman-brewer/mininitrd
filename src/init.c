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
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/sysmacros.h>
#include <sys/reboot.h>

#include "init.h"

/* ------------------------------------------------------------------ */
/* global state                                                        */
/* ------------------------------------------------------------------ */

static int    sinks[16];      /* fds everything is logged to            */
static int    nsinks = 0;
static int    kmsg_fd = -1;
static int    log_fd = -1;    /* /tmp/kerneltest.log once tmpfs is up    */
int           g_pass = 0, g_fail = 0, g_skip = 0;
int           g_quiet = 0;
int           g_scale = 4;    /* loop multiplier: fast=1 all=4 stress=32 */
int           g_loop = 0;
int           g_poweroff = 0, g_reboot = 0, g_panic_fail = 0;
int           g_timeout = 0;
double        g_boot;         /* CLOCK_BOOTTIME at start */

double now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void sink_write(const char *s, size_t n)
{
    int i;
    if (n == 0) return;
    for (i = 0; i < nsinks; i++) {
        ssize_t r = write(sinks[i], s, n);
        (void)r;
    }
}

void logts(const char *fmt, ...)
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

void add_sink(int fd)
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

int write_all(int fd, const void *buf, size_t n)
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

size_t read_file(const char *path, char *buf, size_t size)
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

int str_has(const char *hay, const char *needle)
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

void open_console_sinks(void)
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

void open_kmsg_sink(void)
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

void mount_fs(const char *src, const char *tgt, const char *type,
              unsigned long flags, const char *opts)
{
    if (mount(src, tgt, type, flags, opts) != 0) {
        if (errno != EBUSY && errno != EINVAL && errno != EACCES)
            logts("init: mount %s on %s failed: %s\n",
                  type, tgt, strerror(errno));
    }
}

void setup_dev(void)
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

void parse_cmdline(void)
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
uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ================================================================== */
/*  suite / main                                                       */
/* ================================================================== */

void run_suite(void)
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
    RUN_TEST(getrandom);
    RUN_TEST(epoll);
    RUN_TEST(eventfd);
    RUN_TEST(timerfd);
    RUN_TEST(signalfd);
    RUN_TEST(sysv_ipc);
    RUN_TEST(shm);
    RUN_TEST(semaphore);
    RUN_TEST(futex);
    RUN_TEST(proc_self);
    RUN_TEST(readdir);
    RUN_TEST(rlimit);
    RUN_TEST(clock_res);
    RUN_TEST(fd_dup);
    RUN_TEST(poll_select);
    RUN_TEST(sendfile);
    RUN_TEST(splice);
    RUN_TEST(inotify);
    RUN_TEST(net_udp);
    RUN_TEST(unix_dgram);
    RUN_TEST(statvfs);
    RUN_TEST(madvise);
    RUN_TEST(mlockall);
    RUN_TEST(fallocate);
    RUN_TEST(process_grp);
    RUN_TEST(getcpu);
    RUN_TEST(fadvise);
    RUN_TEST(pty);
    RUN_TEST(framebuffer);
    RUN_TEST(perms);
    RUN_TEST(ramify_regression);
}

void print_summary(void)
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

void end_behavior(void)
{
    print_summary();

    if (g_fail > 0 && g_panic_fail) {
        int fd = open("/proc/sysrq-trigger", O_WRONLY);
        logts("panic_on_fail: triggering kernel panic\n");
        if (fd >= 0) {
            if (write(fd, "c", 1) != 1) {} /* best-effort sysrq */
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
        if (write(STDOUT_FILENO, "EXECOK", 6) != 6) {} /* marker for exec self-test */
        _exit(0);
    }

    g_boot = now_s();

    /* mounts first: everything below relies on them */
    mkdir("/proc", 0555); mkdir("/sys", 0555);
    mount_fs("proc", "/proc", "proc", 0, "");
    mount_fs("sysfs", "/sys", "sysfs", 0, "");
    setup_dev();
    mount_fs("tmpfs", "/tmp", "tmpfs", 0, "mode=1777,size=64M");

    /* pseudo-filesystems used by some tests; a failed mount is fine --
     * the tests that need them degrade to SKIP */
    mkdir("/dev/pts", 0755);
    mount_fs("devpts", "/dev/pts", "devpts", 0, "mode=0620");
    mkdir("/dev/shm", 01777);
    mount_fs("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777");
    mkdir("/dev/mqueue", 0755);
    mount_fs("mqueue", "/dev/mqueue", "mqueue", 0, "");

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

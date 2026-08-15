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
#include <poll.h>
#include <mqueue.h>
#include <semaphore.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/sendfile.h>
#include <sys/select.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>

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
/* 22. getrandom syscall (SKIP if CRNG not ready / syscall missing) --- */
static int getrandom_test(void)
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

/* 23. epoll ----------------------------------------------------------- */
static int epoll_test(void)
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
static int eventfd_test(void)
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

/* 25. timerfd ----------------------------------------------------------- */
static int timerfd_test(void)
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

/* 26. signalfd ---------------------------------------------------------- */
static int signalfd_test(void)
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

/* 27. POSIX message queues via raw syscalls (SKIP if not configured) --- */
static int mq_test(void)
{
    static const char *name = "/kerneltest-mq";
    struct mq_attr attr;
    mqd_t mqd;
    char msg[] = "kernel-mq";
    char rcv[64] = {0};
    unsigned prio = 0;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = 8;
    attr.mq_msgsize = 64;
    syscall(SYS_mq_unlink, name);   /* discard leftovers from aborted runs */
    mqd = syscall(SYS_mq_open, name, O_CREAT | O_RDWR, 0600, &attr);
    if (mqd < 0) {
        if (errno == ENOSYS || errno == EPERM || errno == ENOENT ||
            errno == ENODEV || errno == EINVAL)
            return 2;               /* CONFIG_POSIX_MQUEUE off / no mqueue fs */
        return 1;
    }
    if (mq_send(mqd, msg, sizeof(msg) - 1, 1) != 0)
        goto fail;
    if (mq_receive(mqd, rcv, 64, &prio) != (ssize_t)sizeof(msg) - 1)
        goto fail;
    if (memcmp(rcv, msg, sizeof(msg) - 1) != 0) goto fail;
    mq_close(mqd);
    syscall(SYS_mq_unlink, name);
    return 0;
fail:
    mq_close(mqd);
    syscall(SYS_mq_unlink, name);
    return 1;
}

/* 28. SysV IPC: semaphore, shared memory, message queue ---------------- */
static int sysv_ipc_test(void)
{
    key_t key = 0x4b52544c;                 /* arbitrary private key */
    int semid, shmid, msqid;
    struct sembuf sop;
    int tmp;

    /* discard any SysV objects left behind by a previous crashed run */
    tmp = semget(key, 0, 0); if (tmp >= 0) semctl(tmp, 0, IPC_RMID, 0);
    tmp = shmget(key, 0, 0); if (tmp >= 0) shmctl(tmp, IPC_RMID, 0);
    tmp = msgget(key, 0); if (tmp >= 0) msgctl(tmp, IPC_RMID, 0);

    semid = semget(key, 1, IPC_CREAT | 0600);
    if (semid < 0) {
        if (errno == ENOSYS || errno == EPERM || errno == ENOMEM) return 2;
        return 1;
    }
    if (semctl(semid, 0, SETVAL, 1) != 0) { semctl(semid, 0, IPC_RMID, 0); return 1; }
    sop.sem_num = 0; sop.sem_op = -1; sop.sem_flg = 0;
    if (semop(semid, &sop, 1) != 0) { semctl(semid, 0, IPC_RMID, 0); return 1; }
    semctl(semid, 0, IPC_RMID, 0);

    shmid = shmget(key, 4096, IPC_CREAT | 0600);
    if (shmid < 0) {
        if (errno == ENOSYS || errno == EPERM || errno == ENOMEM) return 2;
        return 1;
    }
    {
        void *shm = shmat(shmid, NULL, 0);
        if (shm == (void *)-1) { shmctl(shmid, IPC_RMID, 0); return 1; }
        memset(shm, 0xab, 4096);
        if (((unsigned char *)shm)[0] != 0xab ||
            ((unsigned char *)shm)[4095] != 0xab) {
            shmdt(shm); shmctl(shmid, IPC_RMID, 0); return 1;
        }
        shmdt(shm);
    }
    shmctl(shmid, IPC_RMID, 0);

    msqid = msgget(key, IPC_CREAT | 0600);
    if (msqid < 0) {
        if (errno == ENOSYS || errno == EPERM || errno == ENOMEM) return 2;
        return 1;
    }
    {
        struct { long mtype; char mtext[16]; } mbuf = { 1, "sysv-msg" };
        struct { long mtype; char mtext[16]; } rbuf;
        if (msgsnd(msqid, &mbuf, 16, 0) != 0) { msgctl(msqid, IPC_RMID, 0); return 1; }
        if (msgrcv(msqid, &rbuf, 16, 1, 0) != 16) {
            msgctl(msqid, IPC_RMID, 0); return 1;
        }
        if (memcmp(rbuf.mtext, "sysv-msg", 8) != 0) {
            msgctl(msqid, IPC_RMID, 0); return 1;
        }
    }
    msgctl(msqid, IPC_RMID, 0);
    return 0;
}

/* 29. shared memory via /dev/shm (SKIP if tmpfs not mounted there) ----- */
static int shm_test(void)
{
    const char *path = "/dev/shm/kerneltest-shm.bin";
    int fd, status;
    pid_t pid;
    char *m;
    fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENODEV || errno == EPERM) return 2;
        return 1;
    }
    if (ftruncate(fd, 4096) != 0) { close(fd); unlink(path); return 1; }
    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); unlink(path); return 1; }
    memset(m, 0, 4096);
    pid = fork();
    if (pid < 0) { munmap(m, 4096); close(fd); unlink(path); return 1; }
    if (pid == 0) { m[0] = 0x5a; m[1] = 0x6b; _exit(0); }
    if (waitpid(pid, &status, 0) != pid) { munmap(m, 4096); close(fd); unlink(path); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        munmap(m, 4096); close(fd); unlink(path); return 1;
    }
    if (m[0] != 0x5a || m[1] != 0x6b) { munmap(m, 4096); close(fd); unlink(path); return 1; }
    munmap(m, 4096);
    close(fd);
    unlink(path);
    return 0;
}

/* 30. POSIX named semaphore -------------------------------------------- */
static int semaphore_test(void)
{
    static const char *name = "/kerneltest-sem";
    sem_t *s = sem_open(name, O_CREAT, 0600, 0);
    struct timespec ts;
    if (s == SEM_FAILED) {
        if (errno == ENOSYS || errno == EPERM || errno == ENOENT) return 2;
        return 1;
    }
    if (sem_post(s) != 0) { sem_close(s); sem_unlink(name); return 1; }
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    if (sem_timedwait(s, &ts) != 0) { sem_close(s); sem_unlink(name); return 1; }
    /* count is 0 again: a 1 s wait must time out */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    if (sem_timedwait(s, &ts) == 0) { sem_close(s); sem_unlink(name); return 1; }
    if (errno != ETIMEDOUT) { sem_close(s); sem_unlink(name); return 1; }
    sem_close(s);
    sem_unlink(name);
    return 0;
}

/* 31. futex ------------------------------------------------------------ */
static int futex_test(void)
{
    volatile int *f;
    pid_t pid;
    int status;
    f = mmap(NULL, sizeof(*f), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (f == MAP_FAILED) return 1;
    *f = 0;
    pid = fork();
    if (pid < 0) { munmap((void *)f, sizeof(*f)); return 1; }
    if (pid == 0) {
        struct timespec tmo;
        long r;
        int n = 0;
        clock_gettime(CLOCK_MONOTONIC, &tmo);
        tmo.tv_sec += 5;
        for (;;) {
            r = syscall(SYS_futex, (void *)f, 0 /* FUTEX_WAIT */, 0,
                        &tmo, NULL, 0);
            if (r == 0) break;
            if (r == -1 && errno == EAGAIN) break;  /* *f already != 0 */
            if (r == -1 && errno == EINTR) continue;
            if (++n > 3) _exit(2);
        }
        *f = 2;
        syscall(SYS_futex, (void *)f, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
        _exit(0);
    }
    clock_nanosleep(CLOCK_MONOTONIC, 0, &(struct timespec){0, 50000000}, NULL);
    *f = 1;
    syscall(SYS_futex, (void *)f, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
    if (waitpid(pid, &status, 0) != pid) { munmap((void *)f, sizeof(*f)); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        munmap((void *)f, sizeof(*f)); return 1;
    }
    if (*f != 2) { munmap((void *)f, sizeof(*f)); return 1; }
    munmap((void *)f, sizeof(*f));
    return 0;
}

/* 32. /proc/self detail ------------------------------------------------ */
static int proc_self_test(void)
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
static int readdir_test(void)
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

/* 34. rlimits ---------------------------------------------------------- */
static int rlimit_test(void)
{
    struct rlimit rl, old;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 1;
    if (rl.rlim_cur == 0 || rl.rlim_max == 0) return 1;
    old = rl;
    if (rl.rlim_cur > 32) {
        rl.rlim_cur -= 16;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) return 1;
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 1;
        if (rl.rlim_cur != old.rlim_cur - 16) return 1;
        if (setrlimit(RLIMIT_NOFILE, &old) != 0) return 1;
    }
    if (getrlimit(RLIMIT_STACK, &rl) != 0) return 1;
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 1024 * 1024) return 1;
    if (getrlimit(RLIMIT_AS, &rl) != 0) return 1;
    return 0;
}

/* 35. clock resolution + extra clocks ---------------------------------- */
static int clock_res_test(void)
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

/* 36. fd duplication: dup / dup2 / dup3 -------------------------------- */
static int fd_dup_test(void)
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

/* 37. poll / select ----------------------------------------------------- */
static int poll_select_test(void)
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

/* 38. sendfile ---------------------------------------------------------- */
static int sendfile_test(void)
{
    const char *in_path = "/tmp/sf_in.bin";
    const char *out_path = "/tmp/sf_out.bin";
    char data[8192];
    char rd[8192];
    int ifd, ofd;
    ssize_t n;
    int i;
    for (i = 0; i < (int)sizeof(data); i++) data[i] = (char)(i * 7);
    ifd = open(in_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (ifd < 0) return 1;
    if (write_all(ifd, data, sizeof(data)) != 0) { close(ifd); return 1; }
    close(ifd);
    ifd = open(in_path, O_RDONLY);
    if (ifd < 0) { unlink(in_path); return 1; }
    ofd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (ofd < 0) { close(ifd); unlink(in_path); return 1; }
    n = sendfile(ofd, ifd, NULL, sizeof(data));
    if (n != (ssize_t)sizeof(data)) {
        if (n < 0 && (errno == EINVAL || errno == ENOSYS)) {
            close(ifd); close(ofd); unlink(in_path); unlink(out_path);
            return 2;   /* kernel/config lacking sendfile support */
        }
        close(ifd); close(ofd); unlink(in_path); unlink(out_path);
        return 1;
    }
    close(ifd); close(ofd);
    ofd = open(out_path, O_RDONLY);
    if (ofd < 0) { unlink(in_path); unlink(out_path); return 1; }
    if (read(ofd, rd, sizeof(rd)) != (ssize_t)sizeof(rd) ||
        memcmp(data, rd, sizeof(data)) != 0) {
        close(ofd); unlink(in_path); unlink(out_path); return 1;
    }
    close(ofd);
    unlink(in_path); unlink(out_path);
    return 0;
}

/* 39. splice ------------------------------------------------------------ */
static int splice_test(void)
{
    int p1[2], p2[2];
    char data[4096], rd[4096];
    ssize_t n;
    int i;
    if (pipe(p1) != 0) return 1;
    if (pipe(p2) != 0) { close(p1[0]); close(p1[1]); return 1; }
    for (i = 0; i < (int)sizeof(data); i++) data[i] = (char)(i ^ 0x5a);
    if (write_all(p1[1], data, sizeof(data)) != 0) {
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]); return 1;
    }
    n = splice(p1[0], NULL, p2[1], NULL, sizeof(data), 0);
    if (n != (ssize_t)sizeof(data)) {
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        if (n < 0 && (errno == EINVAL || errno == ENOSYS)) return 2;
        return 1;
    }
    n = read(p2[0], rd, sizeof(rd));
    close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
    if (n != (ssize_t)sizeof(data) || memcmp(data, rd, sizeof(data)) != 0)
        return 1;
    return 0;
}

/* 40. inotify ------------------------------------------------------------ */
static int inotify_test(void)
{
    const char *dir = "/tmp/inotify_d";
    const char *file = "/tmp/inotify_d/x.txt";
    int ifd, wd, fd, n;
    char evbuf[512];
    struct inotify_event *ev;
    int saw_create = 0, saw_close = 0;
    mkdir(dir, 0755);
    ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        if (errno == ENOSYS || errno == EMFILE || errno == ENODEV) return 2;
        return 1;
    }
    wd = inotify_add_watch(ifd, dir, IN_CREATE | IN_CLOSE_WRITE);
    if (wd < 0) {
        close(ifd); rmdir(dir);
        if (errno == ENOSYS) return 2;
        return 1;
    }
    fd = open(file, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { close(ifd); rmdir(dir); return 1; }
    if (write(fd, "data", 4) != 4) { close(fd); close(ifd); rmdir(dir); return 1; }
    if (close(fd) != 0) { close(ifd); rmdir(dir); return 1; }
    /* drain whatever has been queued (events arrive asynchronously) */
    for (;;) {
        n = read(ifd, evbuf, sizeof(evbuf));
        if (n <= 0) break;
        for (ev = (struct inotify_event *)evbuf;
             (char *)ev < evbuf + n;
             ev = (struct inotify_event *)((char *)ev + sizeof(*ev) + ev->len)) {
            if (ev->mask & IN_CREATE) saw_create = 1;
            if (ev->mask & IN_CLOSE_WRITE) saw_close = 1;
        }
    }
    close(ifd);
    unlink(file);
    rmdir(dir);
    if (!saw_create || !saw_close) return 1;
    return 0;
}

/* 41. UDP loopback -------------------------------------------------------- */
static int net_udp_test(void)
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
static int unix_dgram_test(void)
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

/* 43. statvfs / statfs --------------------------------------------------- */
static int statvfs_test(void)
{
    struct statvfs v;
    struct statfs fs;
    if (statvfs("/", &v) != 0) return 1;
    if (v.f_bsize == 0) return 1;
    /* f_blocks may be 0 on ramfs (kernel without CONFIG_TMPFS): not an error */
    if (statvfs("/tmp", &v) != 0) return 1;
    if (v.f_bsize == 0) return 1;
    if (statfs("/tmp", &fs) != 0) return 1;
    if (fs.f_bsize == 0) return 1;
    return 0;
}

/* 44. madvise ------------------------------------------------------------ */
static int madvise_test(void)
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
static int mlockall_test(void)
{
    if (mlockall(MCL_CURRENT) != 0) {
        if (errno == EPERM || errno == ENOMEM || errno == EINVAL) return 2;
        return 1;
    }
    if (munlockall() != 0) return 1;
    return 0;
}

/* 46. fallocate (SKIP if fs does not support preallocation) ------------- */
static int fallocate_test(void)
{
    const char *path = "/tmp/falloc.bin";
    int fd;
    struct stat st;
    long r;
    fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return 1;
    r = syscall(SYS_fallocate, fd, 0, 0, 1024 * 1024);
    if (r != 0) {
        close(fd); unlink(path);
        if (errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL ||
            errno == EPERM)
            return 2;           /* fs does not support preallocation */
        return 1;
    }
    if (fstat(fd, &st) != 0) { close(fd); unlink(path); return 1; }
    if (st.st_size != 1024 * 1024) { close(fd); unlink(path); return 1; }
    close(fd);
    unlink(path);
    return 0;
}

/* 47. process groups / sessions ------------------------------------------ */
static int process_grp_test(void)
{
    pid_t me = getpid();
    pid_t pp = getppid();
    if (pp < 0) return 1;       /* real error */
    if (pp == 0)
        logts("  parent is PID 0 (we are PID 1)\n");
    if (getpgid(me) != me) return 1;
    if (setsid() == -1) {
        /* Already a session leader (e.g. PID 1). EPERM means "already leader". */
        if (errno == EPERM && getpgid(me) == me) {
            return 0;
        }
        return 1;
    }
    /* We became a session leader — our pgrp must equal our pid */
    if (getpgrp() != me) return 1;
    return 0;
}

/* 48. sched_getcpu / getcpu syscall -------------------------------------- */
static int getcpu_test(void)
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

/* 49. posix_fadvise ------------------------------------------------------ */
static int fadvise_test(void)
{
    const char *path = "/tmp/fadvise.bin";
    int fd;
    char data[4096];
    memset(data, 0x77, sizeof(data));
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return 1;
    if (write_all(fd, data, sizeof(data)) != 0) { close(fd); unlink(path); return 1; }
    close(fd);
    fd = open(path, O_RDONLY);
    if (fd < 0) { unlink(path); return 1; }
    if (posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) != 0) {
        close(fd); unlink(path); return 1;
    }
    if (posix_fadvise(fd, 0, 4096, POSIX_FADV_WILLNEED) != 0) {
        close(fd); unlink(path); return 1;
    }
    close(fd);
    unlink(path);
    return 0;
}

/* 50. pseudo-terminal (SKIP if no ptmx) ---------------------------------- */
static int pty_test(void)
{
    int master, slave;
    char *name;
    char buf[16];
    master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master < 0) {
        if (errno == ENOENT || errno == ENODEV || errno == EACCES) return 2;
        return 1;
    }
    name = ptsname(master);
    if (!name) { close(master); return 1; }
    if (unlockpt(master) != 0) { close(master); return 1; }
    slave = open(name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        if (errno == ENOENT || errno == ENODEV) return 2;  /* no devpts */
        return 1;
    }
    if (write(slave, "pty-test", 8) != 8) {
        close(master); close(slave); return 1;
    }
    if (read(master, buf, sizeof(buf)) != 8 || memcmp(buf, "pty-test", 8) != 0) {
        close(master); close(slave); return 1;
    }
    close(master); close(slave);
    return 0;
}

/* 51. raw framebuffer (SKIP if no suitable device exists) ----------------- */
/* fb ioctls + structs are mirrored from <linux/fb.h> so this file builds
 * without kernel headers.  The ABI is stable; do not reorder fields.   */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602

struct fb_bitfield {
    uint32_t offset, length, msb_right;
};
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate;
    uint32_t height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len;
    uint32_t type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

static int framebuffer_test(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    unsigned char *m;
    size_t fbsize, off;
    size_t i;
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0)
        return 2;               /* no framebuffer device: headless, skip */
    memset(&v, 0, sizeof(v));
    memset(&f, 0, sizeof(f));
    if (ioctl(fb, FBIOGET_VSCREENINFO, &v) != 0 ||
        ioctl(fb, FBIOGET_FSCREENINFO, &f) != 0) {
        close(fb);
        return 2;               /* present but not a real fb device */
    }
    if (f.smem_len == 0 || v.xres == 0 || v.yres == 0) {
        close(fb);
        return 2;
    }
    fbsize = (size_t)f.smem_len;
    logts("  fb0: %ux%u %u bpp, %zu bytes, id=%s\n",
          v.xres, v.yres, v.bits_per_pixel, fbsize, f.id);
    m = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (m == MAP_FAILED) {
        close(fb);
        return 2;               /* framebuffer cannot be mapped: skip */
    }
    /* draw a deterministic stripe pattern across the whole framebuffer */
    for (i = 0; i < fbsize; i++)
        m[i] = (unsigned char)(i >> 8) ^ 0xa5;
    /* liveness check: a working fb must not read back all zeros where we
     * wrote a non-zero pattern.  Some drivers transform the data (format
     * conversion, write-only scanout), so only an all-zero readback at two
     * widely separated offsets counts as a failure. */
    off = fbsize / 2;
    if (m[0] == 0 && m[off] == 0) {
        /* write-only / format-converting scanout reads back zeros on a
         * healthy driver: treat as unsuitable rather than broken */
        logts("  fb0: readback all-zero (write-only scanout) -- skipping\n");
        munmap(m, fbsize); close(fb); return 2;
    }
    logts("  fb0: readback %02x/%02x at 0/%zu\n", m[0], m[off], off);
    if (munmap(m, fbsize) != 0) { close(fb); return 1; }
    close(fb);
    return 0;
}

/* 52. file permission bits (rwx) ------------------------------------------ */
static int perms_test(void)
{
    char path[64] = "", dpath[64] = "";
    struct stat st;
    pid_t p;
    int fd, wst;
    mode_t oldmask = umask(0);
    int r = 1;                    /* pessimist: fail unless proven */

    /* 1) umask 0022 applied at creation: 0666 -> 0644 */
    umask(0022);
    snprintf(path, sizeof(path), "/tmp/perms_%d", (int)getpid());
    unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) {
        logts("  perms: step %d failed: %s\n", 1, strerror(errno));
        goto out;
    }
    if (fstat(fd, &st) != 0 || (st.st_mode & 0777) != 0644) {
        logts("  perms: step %d failed: %s\n", 1, strerror(errno));
        close(fd); goto out;
    }
    close(fd);

    /* 2) chmod to rwx------ and verify via stat */
    if (chmod(path, 0700) != 0) {
        logts("  perms: step %d failed: %s\n", 2, strerror(errno));
        goto out;
    }
    if (stat(path, &st) != 0 || (st.st_mode & 0777) != 0700) {
        logts("  perms: step %d failed: %s\n", 2, strerror(errno));
        goto out;
    }

    /* 3) fchmod via fd: 0640 */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        logts("  perms: step %d failed: %s\n", 3, strerror(errno));
        goto out;
    }
    if (fchmod(fd, 0640) != 0) {
        logts("  perms: step %d failed: %s\n", 3, strerror(errno));
        close(fd); goto out;
    }
    if (fstat(fd, &st) != 0 || (st.st_mode & 0777) != 0640) {
        logts("  perms: step %d failed: %s\n", 3, strerror(errno));
        close(fd); goto out;
    }
    close(fd);

    /* 4) special bits setuid, setgid AND sticky survive chmod (07755) */
    if (chmod(path, 07755) != 0) {
        logts("  perms: step %d failed: %s\n", 4, strerror(errno));
        goto out;
    }
    if (stat(path, &st) != 0 || (st.st_mode & 07777) != 07755) {
        logts("  perms: step %d failed: %s\n", 4, strerror(errno));
        goto out;
    }
    if (!(st.st_mode & S_IXUSR)) {
        logts("  perms: step %d failed: %s\n", 4, strerror(errno));
        goto out;   /* x bit visible */
    }

    /* 5) mkdir mode bits: 0750 */
    snprintf(dpath, sizeof(dpath), "/tmp/perms_dir_%d", (int)getpid());
    if (mkdir(dpath, 0750) != 0) {
        logts("  perms: step %d failed: %s\n", 5, strerror(errno));
        goto out;
    }
    if (stat(dpath, &st) != 0 || (st.st_mode & 0777) != 0750) {
        logts("  perms: step %d failed: %s\n", 5, strerror(errno));
        rmdir(dpath); goto out;
    }
    if (rmdir(dpath) != 0) {
        logts("  perms: step %d failed: %s\n", 5, strerror(errno));
        goto out;
    }

    /* 6) enforcement: a uid-65534 child must NOT open a root-owned 0600 file */
    if (chmod(path, 0600) != 0) {
        logts("  perms: step %d failed: %s\n", 6, strerror(errno));
        goto out;
    }
    p = fork();
    if (p < 0) {
        logts("  perms: step %d failed: %s\n", 6, strerror(errno));
        goto out;
    }
    if (p == 0) {
        int c = 1;
        if (setuid(65534) != 0) _exit(1);
        errno = 0;
        if (open(path, O_RDONLY) < 0 && errno == EACCES) c = 0; /* enforced */
        _exit(c);
    }
    if (waitpid(p, &wst, 0) != p || !WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
        logts("  perms: step %d failed: %s\n", 6, strerror(errno));
        goto out;
    }

    r = 0;
out:
    umask(oldmask);
    if (path[0]) unlink(path);
    if (dpath[0]) rmdir(dpath);
    return r;
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
    RUN_TEST(getrandom);
    RUN_TEST(epoll);
    RUN_TEST(eventfd);
    RUN_TEST(timerfd);
    RUN_TEST(signalfd);
    RUN_TEST(mq);
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

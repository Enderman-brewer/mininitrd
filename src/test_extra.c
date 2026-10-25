#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#include "init.h"

/* Tests here follow the suite convention: 0 = pass, 1 = fail, 2 = skip.
 * Anything that changes namespaces, cwd, signal dispositions, affinity or
 * credentials runs inside a forked child via in_child(), so that state
 * cannot leak into the rest of the suite. */

/* Run fn in a forked child.  The child must _exit() with 0 (pass),
 * 2 (skip) or anything else (fail).  Death by signal counts as fail. */
static int in_child(int (*fn)(void))
{
    pid_t pid = fork();
    int st, r;

    if (pid < 0)
        return 1;
    if (pid == 0)
        _exit(fn());
    if (waitpid(pid, &st, 0) != pid)
        return 1;
    if (!WIFEXITED(st)) {
        logts("  child killed by signal %d (%s)\n",
              WTERMSIG(st), strsignal(WTERMSIG(st)));
        return 1;
    }
    r = WEXITSTATUS(st);
    if (r == 0) return 0;
    if (r == 2) return 2;
    return 1;
}

/* 52. memfd_create + MAP_SHARED visibility across fork ------------------ */
int memfd_test(void)
{
    unsigned char *p;
    pid_t pid;
    int fd, st, r = 1;

    fd = memfd_create("kt-memfd", 0);
    if (fd < 0)
        return errno == ENOSYS ? 2 : 1;
    if (ftruncate(fd, 4096) != 0) {
        close(fd);
        return 1;
    }
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return 1;
    }
    memset(p, 0x5a, 4096);

    pid = fork();
    if (pid == 0) {
        int ok = (p[0] == 0x5a && p[4095] == 0x5a);
        p[0] = 0x77;                    /* must be visible to the parent */
        _exit(ok ? 0 : 1);
    }
    if (pid > 0 && waitpid(pid, &st, 0) == pid &&
        WIFEXITED(st) && WEXITSTATUS(st) == 0 && p[0] == 0x77)
        r = 0;
    else
        logts("  memfd: shared mapping not visible across fork\n");

    munmap(p, 4096);
    close(fd);
    return r;
}

/* 53. mremap grow and shrink with contents preserved -------------------- */
int mremap_test(void)
{
    const size_t small = 4096 * 4, big = 4096 * 16;
    unsigned char *p, *q;
    size_t i;

    p = mmap(NULL, small, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 1;
    for (i = 0; i < small; i++)
        p[i] = (unsigned char)(i * 13);

    q = mremap(p, small, big, MREMAP_MAYMOVE);
    if (q == MAP_FAILED) {
        munmap(p, small);
        return 1;
    }
    for (i = 0; i < small; i++)
        if (q[i] != (unsigned char)(i * 13)) {
            munmap(q, big);
            return 1;
        }
    q[big - 1] = 0xee;                  /* the grown tail must be writable */
    if (q[big - 1] != 0xee) {
        munmap(q, big);
        return 1;
    }

    p = mremap(q, big, 4096, 0);        /* shrink in place */
    if (p == MAP_FAILED)
        return 1;
    for (i = 0; i < 4096; i++)
        if (p[i] != (unsigned char)(i * 13)) {
            munmap(p, 4096);
            return 1;
        }
    munmap(p, 4096);
    return 0;
}

/* 54. write to a PROT_READ page must fault ------------------------------ */
int mprotect_test(void)
{
    pid_t pid = fork();
    int st;

    if (pid < 0)
        return 1;
    if (pid == 0) {
        volatile unsigned char *p = mmap(NULL, 4096, PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            _exit(2);
        p[0] = 1;                       /* must raise SIGSEGV */
        _exit(1);                       /* reached only if it did not */
    }
    if (waitpid(pid, &st, 0) != pid)
        return 1;
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV)
        return 0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 2)
        return 2;
    logts("  write to PROT_READ page did not fault\n");
    return 1;
}

/* 55. sched_setaffinity pins the task to CPU 0 ------------------------- */
static int affinity_child(void)
{
    cpu_set_t set, got;

    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        return 1;
    CPU_ZERO(&got);
    if (sched_getaffinity(0, sizeof got, &got) != 0)
        return 1;
    if (!CPU_ISSET(0, &got) || CPU_COUNT(&got) != 1)
        return 1;
    if (sched_getcpu() != 0)
        return 1;
    return 0;
}

int affinity_test(void)
{
    return in_child(affinity_child);
}

/* 56. flock() and fcntl() record locks conflict across processes -------- */
int locks_test(void)
{
    char path[64];
    struct flock fl;
    pid_t pid;
    int fd, st, r = 1;

    snprintf(path, sizeof path, "/tmp/kt-lock-%d", (int)getpid());
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return 1;

    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;

    if (flock(fd, LOCK_EX | LOCK_NB) != 0 || fcntl(fd, F_SETLK, &fl) != 0)
        goto out;

    pid = fork();
    if (pid == 0) {
        struct flock f2 = fl;
        int fd2 = open(path, O_RDWR);
        int a, b;

        if (fd2 < 0)
            _exit(1);
        a = flock(fd2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK;
        b = fcntl(fd2, F_SETLK, &f2) == -1 &&
            (errno == EAGAIN || errno == EACCES);
        _exit(a && b ? 0 : 1);
    }
    if (pid > 0 && waitpid(pid, &st, 0) == pid &&
        WIFEXITED(st) && WEXITSTATUS(st) == 0)
        r = 0;
    else
        logts("  locks: child saw no conflict with the parent's lock\n");

out:
    close(fd);
    unlink(path);
    return r;
}

/* 57. hard links, symlinks and rename() semantics ---------------------- */
static ssize_t slurp(const char *p, char *b, size_t n)
{
    ssize_t got;
    int fd = open(p, O_RDONLY);

    if (fd < 0)
        return -1;
    got = read(fd, b, n);
    close(fd);
    return got;
}

int links_test(void)
{
    char dir[64], a[80], b[80], c[80], d[80], e[80], buf[16];
    struct stat s1, s2;
    ssize_t n;
    int fd, r = 1;

    snprintf(dir, sizeof dir, "/tmp/kt-links-%d", (int)getpid());
    snprintf(a, sizeof a, "%s/a", dir);
    snprintf(b, sizeof b, "%s/b", dir);
    snprintf(c, sizeof c, "%s/c", dir);
    snprintf(d, sizeof d, "%s/d", dir);
    snprintf(e, sizeof e, "%s/e", dir);

    if (mkdir(dir, 0700) != 0)
        return 1;

    fd = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        goto out;
    if (write(fd, "one", 3) != 3) {
        close(fd);
        goto out;
    }
    close(fd);

    if (link(a, b) != 0)
        goto out;
    if (stat(a, &s1) != 0 || stat(b, &s2) != 0)
        goto out;
    if (s1.st_ino != s2.st_ino || s1.st_nlink != 2)
        goto out;

    if (symlink("a", c) != 0)
        goto out;
    n = readlink(c, buf, sizeof buf - 1);
    if (n != 1 || buf[0] != 'a')
        goto out;

    if (rename(b, d) != 0)
        goto out;
    if (access(b, F_OK) == 0 || errno != ENOENT)
        goto out;

    /* rename over an existing name replaces it atomically */
    fd = open(e, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        goto out;
    if (write(fd, "two", 3) != 3) {
        close(fd);
        goto out;
    }
    close(fd);
    if (rename(e, a) != 0)
        goto out;
    n = slurp(a, buf, sizeof buf);
    if (n != 3 || memcmp(buf, "two", 3) != 0)
        goto out;

    r = 0;
out:
    if (r != 0)
        logts("  links: link/symlink/rename check failed\n");
    unlink(a); unlink(b); unlink(c); unlink(d); unlink(e);
    rmdir(dir);
    return r;
}

/* 58. user.* extended attributes on tmpfs ------------------------------ */
int xattr_test(void)
{
    char path[64], buf[8];
    int fd, r = 1;
    ssize_t n;

    snprintf(path, sizeof path, "/tmp/kt-xattr-%d", (int)getpid());
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        return 1;
    close(fd);

    if (setxattr(path, "user.kt", "v", 1, 0) != 0) {
        int e = errno;
        unlink(path);
        return (e == ENOTSUP || e == EOPNOTSUPP) ? 2 : 1;
    }
    n = getxattr(path, "user.kt", buf, sizeof buf);
    if (n == 1 && buf[0] == 'v' &&
        removexattr(path, "user.kt") == 0 &&
        getxattr(path, "user.kt", buf, sizeof buf) == -1 && errno == ENODATA)
        r = 0;
    else
        logts("  xattr: set/get/remove round-trip failed\n");

    unlink(path);
    return r;
}

/* 59. fsync / fdatasync return success and data is readable after ------ */
int fsync_test(void)
{
    char path[64];
    unsigned char buf[4096], back[4096];
    int fd, r = 1;
    size_t i;

    snprintf(path, sizeof path, "/tmp/kt-fsync-%d", (int)getpid());
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return 1;

    for (i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)(i * 7);

    if (pwrite(fd, buf, sizeof buf, 0) == (ssize_t)sizeof buf &&
        fsync(fd) == 0 && fdatasync(fd) == 0 &&
        pread(fd, back, sizeof back, 0) == (ssize_t)sizeof back &&
        memcmp(buf, back, sizeof buf) == 0)
        r = 0;
    else
        logts("  fsync: write/sync/read-back failed\n");

    close(fd);
    unlink(path);
    return r;
}

/* 60. UTS namespace isolates hostname ---------------------------------- */
static int uts_child(void)
{
    char h[65];

    if (unshare(CLONE_NEWUTS) != 0)
        return (errno == EPERM || errno == ENOSYS || errno == EINVAL) ? 2 : 1;
    if (sethostname("kt-uts", 6) != 0)
        return 1;
    if (gethostname(h, sizeof h - 1) != 0)
        return 1;
    return strcmp(h, "kt-uts") == 0 ? 0 : 1;
}

int unshare_uts_test(void)
{
    char before[65] = "", after[65] = "";
    int r;

    if (gethostname(before, sizeof before - 1) != 0)
        return 1;
    r = in_child(uts_child);
    if (gethostname(after, sizeof after - 1) != 0)
        return 1;
    if (r == 0 && strcmp(before, after) != 0) {
        logts("  uts: parent hostname changed by child namespace\n");
        return 1;
    }
    return r;
}

/* 61. chroot() confines the process; ".." cannot escape ---------------- */
static char g_jail[64];

static int chroot_child(void)
{
    char cwd[256];

    if (chroot(g_jail) != 0)
        return (errno == EPERM || errno == ENOSYS) ? 2 : 1;
    if (chdir("/") != 0)
        return 1;
    if (access("/tmp", F_OK) == 0)      /* real /tmp must be invisible */
        return 1;
    if (chdir("..") != 0)
        return 1;
    if (getcwd(cwd, sizeof cwd) == NULL || strcmp(cwd, "/") != 0)
        return 1;
    return 0;
}

int chroot_test(void)
{
    int r;

    snprintf(g_jail, sizeof g_jail, "/tmp/kt-jail-%d", (int)getpid());
    if (mkdir(g_jail, 0700) != 0)
        return 1;
    r = in_child(chroot_child);
    rmdir(g_jail);
    return r;
}

/* 62. prctl: process name and no_new_privs ----------------------------- */
static int prctl_child(void)
{
    char name[16] = "";

    if (prctl(PR_SET_NAME, "kt-prctl", 0, 0, 0) != 0)
        return 1;
    if (prctl(PR_GET_NAME, name, 0, 0, 0) != 0)
        return 1;
    if (strcmp(name, "kt-prctl") != 0)
        return 1;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return 1;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
        return 1;
    if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) < 0)
        return 1;
    return 0;
}

int prctl_test(void)
{
    return in_child(prctl_child);
}

/* 63. pidfd_open: pidfd becomes readable when the child exits ---------- */
int pidfd_test(void)
{
    struct pollfd p;
    pid_t pid = fork();
    int pfd, r;

    if (pid < 0)
        return 1;
    if (pid == 0) {
        usleep(50000);
        _exit(0);
    }
    pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd < 0) {
        int e = errno;
        waitpid(pid, NULL, 0);
        return e == ENOSYS ? 2 : 1;
    }
    memset(&p, 0, sizeof p);
    p.fd = pfd;
    p.events = POLLIN;
    r = (poll(&p, 1, 5000) == 1 && (p.revents & POLLIN)) ? 0 : 1;
    close(pfd);
    waitpid(pid, NULL, 0);
    return r;
}

/* 64. sigaltstack: handler runs on the alternate stack ------------------ */
#define ALT_SZ 65536
static volatile sig_atomic_t alt_ok;
static char *alt_lo, *alt_hi;

static void alt_handler(int sig)
{
    char here;
    (void)sig;
    alt_ok = (&here >= alt_lo && &here < alt_hi);
}

static int sigaltstack_child(void)
{
    struct sigaction sa;
    stack_t ss;
    char *mem = malloc(ALT_SZ);

    if (!mem)
        return 1;
    alt_lo = mem;
    alt_hi = mem + ALT_SZ;

    memset(&ss, 0, sizeof ss);
    ss.ss_sp = mem;
    ss.ss_size = ALT_SZ;
    if (sigaltstack(&ss, NULL) != 0)
        return 1;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = alt_handler;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
        return 1;

    raise(SIGUSR1);
    return alt_ok ? 0 : 1;
}

int sigaltstack_test(void)
{
    return in_child(sigaltstack_child);
}

/* 65. SIGCHLD is delivered to a handler when a child exits ------------- */
static volatile sig_atomic_t chld_seen;

static void chld_handler(int sig)
{
    (void)sig;
    chld_seen = 1;
}

static int sigchld_child(void)
{
    struct sigaction sa;
    struct timespec ts = { 0, 1000000 };
    pid_t pid;
    int st, i;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = chld_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        return 1;

    pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0)
        _exit(0);

    for (i = 0; i < 1000 && !chld_seen; i++)
        nanosleep(&ts, NULL);

    if (waitpid(pid, &st, 0) != pid)
        return 1;
    return (chld_seen && WIFEXITED(st)) ? 0 : 1;
}

int sigchld_test(void)
{
    return in_child(sigchld_child);
}

/* 66. blocked signals stay pending and are delivered on unblock -------- */
static volatile sig_atomic_t usr2_count;

static void usr2_handler(int sig)
{
    (void)sig;
    usr2_count++;
}

static int sigmask_child(void)
{
    struct sigaction sa;
    sigset_t blk, pend;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = usr2_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, NULL) != 0)
        return 1;

    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &blk, NULL) != 0)
        return 1;

    raise(SIGUSR2);
    if (usr2_count != 0)
        return 1;
    if (sigpending(&pend) != 0 || !sigismember(&pend, SIGUSR2))
        return 1;

    if (sigprocmask(SIG_UNBLOCK, &blk, NULL) != 0)
        return 1;
    return usr2_count == 1 ? 0 : 1;
}

int sigmask_test(void)
{
    return in_child(sigmask_child);
}

/* 67. CLOCK_MONOTONIC never goes backwards ----------------------------- */
int clock_mono_test(void)
{
    struct timespec prev, cur;
    long i;

    if (clock_gettime(CLOCK_MONOTONIC, &prev) != 0)
        return 1;
    for (i = 0; i < 200000; i++) {
        if (clock_gettime(CLOCK_MONOTONIC, &cur) != 0)
            return 1;
        if (cur.tv_sec < prev.tv_sec ||
            (cur.tv_sec == prev.tv_sec && cur.tv_nsec < prev.tv_nsec)) {
            logts("  clock: CLOCK_MONOTONIC went backwards at iter %ld\n", i);
            return 1;
        }
        prev = cur;
    }
    return 0;
}

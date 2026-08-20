#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <sys/resource.h>

#include "init.h"

/* 34. rlimits ---------------------------------------------------------- */
int rlimit_test(void)
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

/* 16. exec self (full exec path through the kernel) ------------------ */
int exec_test(void)
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
int fork_storm_test(void)
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

/* 47. process groups / sessions ------------------------------------------ */
int process_grp_test(void)
{
    pid_t me = getpid();
    pid_t pp = getppid();
    pid_t mypgrp;

    if (pp < 0) return 1;               /* real error */

    if (pp == 0) {
        /* PID 1 special case: the kernel reports init's parent as PID 0.
         * That is expected for the initial userspace process, not a bug. */
        if (me != 1) return 1;          /* ppid 0 but not PID 1: inconsistent */
        logts("  parent is PID 0 (we are PID 1) -- expected for init\n");

        mypgrp = getpgid(me);
        if (mypgrp < 0) return 1;        /* real syscall error only */

        errno = 0;
        if (setsid() == -1) {
            if (errno != EPERM) return 1;
        } else if (getpgrp() != me) {
            return 1;
        }

        return 0;                       /* PID 1: all good */
    }

    /* Normal child process: the parent must be a real, live process
     * (never PID 0) and the process-group/session invariants must hold. */
    if (pp == me) return 1;             /* cannot be our own parent */
    if (kill(pp, 0) != 0) return 1;     /* signal 0 = pure existence probe */

    if (getpgid(me) < 1) return 1;

    if (setsid() == -1) {
        /* already a session leader -- that is fine */
        if (errno == EPERM && getpgid(me) == me) {
            logts("  already a session leader\n");
            return 0;
        }
        return 1;
    }
    /* became a session leader: our pgrp must equal our pid */
    if (getpgrp() != me) return 1;
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/syscall.h>

#include "init.h"

/* 15. pipe IPC + fork ------------------------------------------------ */
int pipe_ipc_test(void)
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

/* 28. SysV IPC: semaphore, shared memory, message queue ---------------- */
int sysv_ipc_test(void)
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
int shm_test(void)
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
int semaphore_test(void)
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
int futex_test(void)
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

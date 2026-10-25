#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "init.h"

/* 50. pseudo-terminal (SKIP if no ptmx) ---------------------------------- */
int pty_test(void)
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

/* 51. file permission bits (rwx) ------------------------------------------ */
int perms_test(void)
{
    char path[64] = "", dpath[64] = "";
    struct stat st;
    pid_t p;
    int fd, wst;
    mode_t oldmask = umask(0);
    int r = 1;                    /* pessimist: fail unless proven */

    /* mirrored from <linux/fs.h> for the +i bit -- build without kernel headers */
    #define FS_IOC_GETFLAGS  0x80046601
    #define FS_IOC_SETFLAGS  0x40086602
    #define FS_IMMUTABLE_FL  0x00000010

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

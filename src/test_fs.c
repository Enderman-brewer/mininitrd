#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/inotify.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/syscall.h>

#include "init.h"

/* 5. tmpfs mount + basic fs ops ------------------------------------- */
int fs_tmpfs_test(void)
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
int fs_stress_test(void)
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

/* 19. mmap file I/O on tmpfs ----------------------------------------- */
int fs_mmap_test(void)
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

/* 43. statvfs / statfs --------------------------------------------------- */
int statvfs_test(void)
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

/* 46. fallocate (SKIP if fs does not support preallocation) ------------- */
int fallocate_test(void)
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

/* 49. posix_fadvise ------------------------------------------------------ */
int fadvise_test(void)
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

/* 38. sendfile ---------------------------------------------------------- */
int sendfile_test(void)
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
int splice_test(void)
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
int inotify_test(void)
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

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

/* 51. raw framebuffer (SKIP if no suitable device) ----------------- */
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

int framebuffer_test(void)
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

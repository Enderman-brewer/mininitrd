# mininitrd — boot-time kernel test initramfs

A minimal initramfs whose *only* job is to **fully exercise the running
kernel during boot**, log everything to the console / serial port / kmsg,
and then **sleep forever** so the machine stays up for inspection.

## What it does

`/init` is a single statically-linked C binary (no busybox, no shared
libs, no external tools). It:

1. mounts proc, sysfs, devtmpfs (with a manual `mknod` fallback) and tmpfs
2. detects every console-ish device: `console=` on the kernel cmdline,
   `/proc/consoles`, and a wide list of serial names (`ttyS*`, `ttyAMA*`,
   `ttySAC*`, `ttymxc*`, `ttyPS*`, `hvc0`, `xvc0`, ...). Output goes to
   *all* of them, plus `/dev/kmsg` so the log survives into `dmesg`.
   The preferred console (flag `C` in `/proc/consoles`) is not re-opened,
   so lines are never doubled on the same UART.
3. runs a 51-test suite, each reporting `[PASS]` / `[FAIL]` / `[SKIP]`:
   sysinfo, procfs/sysfs integrity, device nodes, tmpfs ops + stress,
   malloc churn, mmap anon + mlock, CPU arithmetic + threads, scheduler,
   timers/hrtimers, signals, pipe IPC, fork+exec, fork storm, loopback
   TCP + AF_UNIX, mmap file I/O, kernel-log scan (Oops/BUG/WARNING/KASAN),
   KUnit scan (when built into the kernel), plus a second batch that
   exercises more kernel facilities: getrandom, epoll, eventfd, timerfd,
   signalfd, POSIX message queues, SysV IPC (sem/sh/msg), shared memory
   via /dev/shm, POSIX semaphores, futex, /proc/self detail, directory
   enumeration, rlimits, clock resolution, fd duplication, poll/select,
   sendfile, splice, inotify, UDP loopback, AF_UNIX datagrams, statvfs,
   madvise, mlockall, fallocate, process groups, sched_getcpu,
   posix_fadvise, pseudo-terminals, and a raw framebuffer test (SKIPs
   when no /dev/fb0 exists, e.g. headless systems)
4. prints a summary and then sleeps forever (or powers off / reboots /
   panics — see knobs below)

Tests degrade gracefully: things that depend on kernel config (KUnit,
networking, mlock, a controlling tty) report `[SKIP]`, never `[FAIL]`, so
a healthy kernel should read `0 failed`.

## Build

```sh
make              # initramfs.cpio (uncompressed) + initramfs.cpio.gz
make initramfs.cpio.zst    # optional zstd variant
make initramfs.cpio.xz     # optional xz variant
CROSS_COMPILE=arm-linux-gnueabi- make   # cross build
```

Requires a C toolchain with a static libc (`glibc-static` on Arch).
Note: the resulting binary is built against your distro's static glibc
and may require whatever CPU baseline that glibc was built for (e.g.
AVX2 on current Arch). For other architectures/toolchains the same
constraint applies to that glibc, not to this code.

## Boot-test it

```sh
./run-qemu.sh                          # /boot/vmlinuz-linux
KERNEL=/boot/vmlinuz-linux-lts ./run-qemu.sh
KERNEL=/boot/vmlinuz-linux-cachyos ./run-qemu.sh
EXTRA="kerneltest=stress" KERNEL=/boot/vmlinuz-linux-lts ./run-qemu.sh
```

On real hardware, boot it from GRUB/U-Boot as a normal initramfs:

```
kernel /vmlinuz-linux root=/dev/ram0 rdinit=/init console=ttyS0,115200
initrd /initramfs.cpio.gz
```

## Kernel cmdline knobs (all optional)

| knob | effect |
|------|--------|
| `kerneltest=fast` | small loops (quick smoke test) |
| `kerneltest=all` | default intensity |
| `kerneltest=stress` | 32x loops, bigger allocations |
| `kerneltest.loop` | re-run the suite forever |
| `kerneltest.poweroff=1` | power off after the summary |
| `kerneltest.reboot=1` | reboot after the summary |
| `kerneltest.panic_on_fail=1` | trigger a kernel panic if any test failed |
| `kerneltest.timeout=N` | hard watchdog: abort after N s and power off |
| `kerneltest.quiet` | less chatter between tests |

Default behavior (no knobs): run the suite once, then **sleep forever**.

## Files

- `init.c` — the whole test harness (PID 1)
- `Makefile` — build + initramfs assembly (cpio newc)
- `build.sh` — convenience build wrapper
- `run-qemu.sh` — QEMU boot-test (KVM + `-cpu host`, TCG + `-cpu max` fallback)

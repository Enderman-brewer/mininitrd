#ifndef INIT_H
#define INIT_H

#include <stdint.h>
#include <stddef.h>

/* global state */
extern double g_boot;
extern int g_pass, g_fail, g_skip, g_quiet, g_scale, g_loop;
extern int g_poweroff, g_reboot, g_panic_fail, g_timeout;

/* logging / framework */
void logts(const char *fmt, ...);
double now_s(void);
void sink_write(const char *s, size_t n);
void add_sink(int fd);
void open_console_sinks(void);
void open_kmsg_sink(void);
void mount_fs(const char *src, const char *tgt, const char *type,
              unsigned long flags, const char *opts);
void setup_dev(void);
void parse_cmdline(void);

/* helpers available to all test files */
int write_all(int fd, const void *buf, size_t n);
size_t read_file(const char *path, char *buf, size_t size);
int str_has(const char *hay, const char *needle);
uint32_t rng(void);

/* suite */
void run_suite(void);
void print_summary(void);
void end_behavior(void);

/* test functions */
int sysinfo_test(void);
int procfs_test(void);
int sysfs_test(void);
int devfs_test(void);
int fs_tmpfs_test(void);
int fs_stress_test(void);
int mem_malloc_test(void);
int mem_mmap_test(void);
int mem_mlock_test(void);
int cpu_calc_test(void);
int cpu_threads_test(void);
int sched_test(void);
int timer_test(void);
int signal_test(void);
int pipe_ipc_test(void);
int exec_test(void);
int fork_storm_test(void);
int net_loopback_test(void);
int fs_mmap_test(void);
int kmsg_scan_test(void);
int kunit_scan_test(void);
int getrandom_test(void);
int epoll_test(void);
int eventfd_test(void);
int timerfd_test(void);
int signalfd_test(void);
int sysv_ipc_test(void);
int shm_test(void);
int semaphore_test(void);
int futex_test(void);
int proc_self_test(void);
int readdir_test(void);
int rlimit_test(void);
int clock_res_test(void);
int fd_dup_test(void);
int poll_select_test(void);
int sendfile_test(void);
int splice_test(void);
int inotify_test(void);
int net_udp_test(void);
int unix_dgram_test(void);
int statvfs_test(void);
int madvise_test(void);
int mlockall_test(void);
int fallocate_test(void);
int process_grp_test(void);
int getcpu_test(void);
int fadvise_test(void);
int pty_test(void);
int framebuffer_test(void);
int perms_test(void);
int ramify_regression_test(void);

#endif

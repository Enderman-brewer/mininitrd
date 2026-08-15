#ifndef INIT_H
#define INIT_H

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

/* suite */
void run_suite(void);
void print_summary(void);
void end_behavior(void);

/* permission test */
int perms_test(void);

#endif

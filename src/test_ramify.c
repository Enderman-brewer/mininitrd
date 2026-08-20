#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "init.h"

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

int ramify_regression_test(void)
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
                (void)read(fd, buf, sizeof(buf));
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

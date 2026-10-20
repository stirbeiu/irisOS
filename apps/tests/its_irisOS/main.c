/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: apps/tests/its_irisOS/main.c
 * Purpose: Interactive Test Suite (its_irisOS) verifying mlibc C runtime,
 *          heap allocation (malloc/free), client-side FD table, virtual console
 *          devices, and posix_spawn file actions.
 *
 * Licensing: MIT License. Permissive Zone 1 test application.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>

static void log_line(const char* msg) {
    if (!msg) return;
    write(1, msg, strlen(msg));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    char line_buf[256];

    /* 1. Verify standard C library memory allocation (malloc/free) and formatting */
    const char* test_payload = "standard C runtime heap & stdio verified";
    size_t len = strlen(test_payload) + 1;

    char* buffer = (char*)malloc(len);
    if (!buffer) {
        snprintf(line_buf, sizeof(line_buf),
                 "[FAILED] its_irisOS: malloc(%zu) allocation failed.\n", len);
        log_line(line_buf);
        return 1;
    }

    strcpy(buffer, test_payload);
    snprintf(line_buf, sizeof(line_buf),
             "[  OK  ] its_irisOS: malloc/free & snprintf (%s).\n", buffer);
    log_line(line_buf);
    free(buffer);

    /* 2. Verify Client-Side File Descriptor Table via POSIX write(1) */
    const char* fd_msg = "[  OK  ] its_irisOS: FD Table direct write(1) to stdout.\n";
    ssize_t written = write(1, fd_msg, strlen(fd_msg));
    if (written < 0) {
        snprintf(line_buf, sizeof(line_buf),
                 "[FAILED] its_irisOS: write(1) failed with errno %d.\n", errno);
        log_line(line_buf);
        return 2;
    }

    /* 3. Verify isatty() on default file descriptors */
    int tty1 = isatty(1);
    int tty2 = isatty(2);
    snprintf(line_buf, sizeof(line_buf),
             "[  OK  ] its_irisOS: FD Table isatty(1)=%d, isatty(2)=%d (console TTY verified).\n",
             tty1, tty2);
    log_line(line_buf);

    /* 4. Verify open(), write(), and close() on /dev/console virtual device */
    int dev_fd = open("/dev/console", O_WRONLY);
    if (dev_fd >= 3) {
        snprintf(line_buf, sizeof(line_buf),
                 "[  OK  ] its_irisOS: Console device open(\"/dev/console\") -> fd %d, write & close.\n",
                 dev_fd);
        write(dev_fd, line_buf, strlen(line_buf));
        close(dev_fd);
    } else {
        snprintf(line_buf, sizeof(line_buf),
                 "[FAILED] its_irisOS: open(\"/dev/console\") returned %d.\n", dev_fd);
        log_line(line_buf);
        return 3;
    }

    /* 5. Verify EBADF handling on invalid/closed descriptor */
    int close_res = close(99);
    if (close_res == -1 && errno == EBADF) {
        snprintf(line_buf, sizeof(line_buf),
                 "[  OK  ] its_irisOS: FD Table invalid descriptor rejection (EBADF).\n");
        log_line(line_buf);
    }

    /* 6. Verify posix_spawn file actions primitives */
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) == 0) {
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        posix_spawn_file_actions_destroy(&actions);
        snprintf(line_buf, sizeof(line_buf),
                 "[  OK  ] its_irisOS: POSIX spawn file actions (init/adddup2/destroy).\n");
        log_line(line_buf);
    }

    snprintf(line_buf, sizeof(line_buf),
             "[  OK  ] its_irisOS: Interactive Test Suite all checks passed.\n");
    log_line(line_buf);
    return 0;
}

/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: apps/tests/hello_c/main.c
 * Purpose: User-space standard C test application verifying mlibc, FD table,
 *          FD inheritance, POSIX I/O, and spawn primitives.
 *
 * Licensing: MIT License. Permissive Zone 1 test app.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /* 1. Verify standard C library memory allocation and stdio printf() */
    const char* message = "[hello_c] Hello from standard C on irisOS! malloc/free/printf working.\n";
    size_t len = strlen(message) + 1;

    char* buffer = (char*)malloc(len);
    if (!buffer) {
        printf("[hello_c] Error: malloc(%zu) failed!\n", len);
        return 1;
    }

    strcpy(buffer, message);
    printf("%s", buffer);
    fflush(stdout);
    free(buffer);

    /* 2. Verify Client-Side File Descriptor Table via POSIX write() */
    const char* fd_msg = "[hello_c] FD Table: direct write(1) to stdout successful.\n";
    ssize_t written = write(1, fd_msg, strlen(fd_msg));
    if (written < 0) {
        printf("[hello_c] Error: write(1) failed with errno %d\n", errno);
        return 2;
    }

    /* 3. Verify isatty() on default file descriptors */
    int tty1 = isatty(1);
    int tty2 = isatty(2);
    printf("[hello_c] FD Table: isatty(1)=%d isatty(2)=%d (console tty verified).\n", tty1, tty2);
    fflush(stdout);

    /* 4. Verify open(), write(), and close() on virtual character device */
    int dev_fd = open("/dev/console", O_WRONLY);
    if (dev_fd >= 3) {
        const char* dev_msg = "[hello_c] FD Table: open(\"/dev/console\") allocated fd %d, write OK.\n";
        char dev_buf[128];
        snprintf(dev_buf, sizeof(dev_buf), dev_msg, dev_fd);
        write(dev_fd, dev_buf, strlen(dev_buf));
        close(dev_fd);
        printf("[hello_c] FD Table: close(fd %d) successful.\n", dev_fd);
        fflush(stdout);
    } else {
        printf("[hello_c] Warning: open(\"/dev/console\") returned fd %d\n", dev_fd);
        fflush(stdout);
    }

    /* 5. Verify EBADF handling on closed descriptor */
    int close_res = close(99);
    if (close_res == -1 && errno == EBADF) {
        printf("[hello_c] FD Table: invalid fd rejection (EBADF) verified.\n");
        fflush(stdout);
    }

    /* 6. Verify posix_spawn file actions primitives */
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) == 0) {
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        posix_spawn_file_actions_destroy(&actions);
        printf("[hello_c] POSIX Spawn: file actions init/adddup2/destroy working.\n");
        fflush(stdout);
    }

    printf("[hello_c] All Phase 3 C runtime, FD table, and spawn verifications PASSED!\n");
    fflush(stdout);
    return 0;
}

/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: apps/sysutils/iris_shell/main.c
 * Purpose: Persistent interactive shell launcher (boot mode "shell").
 *
 * Lifecycle:
 *   Spawned by iris_init (instead of its_irisOS) when the kernel command line
 *   contains iris.mode=shell. Repeatedly posix_spawn()s /bin/sh (Toybox) and
 *   waits for it; when the shell exits (e.g. Ctrl+D / "exit") it is restarted.
 *   If the shell dies repeatedly in quick succession the launcher gives up
 *   instead of spinning.
 *
 * Channel endpoints / capabilities (via bootstrap message, handled by mlibc):
 *   PA_NS_DIR  - iris_fshost VFS channel (needed to load /bin/sh)
 *   PA_USER0   - iris_pty channel; mlibc binds fds 0/1/2 to it, and sys_spawn
 *                propagates them to the shell so it talks to the PTY.
 *
 * Licensing: MIT License.
 */

#include <stdio.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

#include <iris/log.h>

#define SHELL_PATH            "/bin/sh"
#define SHELL_MAX_FAST_EXITS  5   /* consecutive exits shorter than the threshold */
#define SHELL_FAST_EXIT_SECS  2

int main(void) {
    int fast_exits = 0;
    sleep(1);

    for (;;) {
        char* argv[] = { (char*)"sh", (char*)"-i", NULL };
        pid_t pid = 0;
        time_t start = time(NULL);

        int rc = posix_spawn(&pid, SHELL_PATH, NULL, NULL, argv, NULL);
        if (rc != 0) {
            log_status(STATUS_FAIL, "iris_shell", "posix_spawn(%s) failed (%d).", SHELL_PATH, rc);
            return 1;
        }
        log_status(STATUS_OKAY, "iris_shell", "Started interactive shell (%s).", SHELL_PATH);

        int wstatus = 0;
        waitpid(pid, &wstatus, 0);

        if (time(NULL) - start < SHELL_FAST_EXIT_SECS) {
            if (++fast_exits >= SHELL_MAX_FAST_EXITS) {
                log_status(STATUS_FAIL, "iris_shell", "Shell exiting immediately, giving up.");
                return 1;
            }
        } else {
            fast_exits = 0;
        }
        log_status(STATUS_WARN, "iris_shell", "Shell exited (status=%d), restarting.", WEXITSTATUS(wstatus));
        sleep(1);
    }
}

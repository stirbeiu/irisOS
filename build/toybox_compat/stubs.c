#include <errno.h>
#include <stdio.h>
#include <sys/types.h>

long syscall(long number, ...) {
    (void)number;
    errno = ENOSYS;
    return -1;
}

FILE *setmntent(const char *filename, const char *type) {
    (void)filename; (void)type;
    errno = ENOSYS;
    return NULL;
}

struct mntent;
struct mntent *getmntent(FILE *f) {
    (void)f;
    errno = ENOSYS;
    return NULL;
}

int endmntent(FILE *f) {
    (void)f;
    return 1;
}

int inotify_init(void) {
    errno = ENOSYS;
    return -1;
}

int inotify_init1(int flags) {
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int inotify_add_watch(int fd, const char *pathname, unsigned int mask) {
    (void)fd; (void)pathname; (void)mask;
    errno = ENOSYS;
    return -1;
}

int inotify_rm_watch(int fd, int wd) {
    (void)fd; (void)wd;
    errno = ENOSYS;
    return -1;
}

ssize_t getxattr(const char *path, const char *name, void *value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t fgetxattr(int fd, const char *name, void *value, size_t size) {
    (void)fd; (void)name; (void)value; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t listxattr(const char *path, char *list, size_t size) {
    (void)path; (void)list; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t llistxattr(const char *path, char *list, size_t size) {
    (void)path; (void)list; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t flistxattr(int fd, char *list, size_t size) {
    (void)fd; (void)list; (void)size;
    errno = ENOTSUP;
    return -1;
}

int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOTSUP;
    return -1;
}

int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOTSUP;
    return -1;
}

int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    (void)fd; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOTSUP;
    return -1;
}

#include <spawn.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

extern char **environ;

pid_t xpopen_setup(char **argv, int *pipes, void (*callback)(char **argv)) {
    (void)callback;
    int cestnepasun[4];
    memset(cestnepasun, 0, sizeof(cestnepasun));

    if (pipes) {
        for (int i = 0; i < 2; i++) {
            if (pipes[i] == -1) {
                if (pipe(cestnepasun + (2 * i)) != 0) {
                    return -1;
                }
            }
        }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (pipes) {
        if (cestnepasun[0] != 0) {
            posix_spawn_file_actions_addclose(&actions, cestnepasun[1]);
            posix_spawn_file_actions_adddup2(&actions, cestnepasun[0], 0);
        } else if (pipes[0] > 0) {
            posix_spawn_file_actions_adddup2(&actions, pipes[0], 0);
        }

        if (cestnepasun[3] != 0) {
            posix_spawn_file_actions_addclose(&actions, cestnepasun[2]);
            posix_spawn_file_actions_adddup2(&actions, cestnepasun[3], 1);
        } else if (pipes[1] > 1) {
            posix_spawn_file_actions_adddup2(&actions, pipes[1], 1);
        }
    }

    char* default_argv[] = { (char*)"/bin/sh", NULL };
    char** spawn_argv = (argv && *argv) ? argv : default_argv;

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, spawn_argv[0], &actions, NULL, spawn_argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0) {
        if (cestnepasun[0]) close(cestnepasun[0]);
        if (cestnepasun[1]) close(cestnepasun[1]);
        if (cestnepasun[2]) close(cestnepasun[2]);
        if (cestnepasun[3]) close(cestnepasun[3]);
        errno = rc;
        return -1;
    }

    if (pipes) {
        if (cestnepasun[1]) {
            pipes[0] = cestnepasun[1];
            close(cestnepasun[0]);
        }
        if (cestnepasun[2]) {
            pipes[1] = cestnepasun[2];
            close(cestnepasun[3]);
        }
    }

    return pid;
}

pid_t xpopen(char **argv, int *pipes) {
    return xpopen_setup(argv, pipes, NULL);
}

pid_t xpopen_both(char **argv, int *pipes) {
    return xpopen_setup(argv, pipes, NULL);
}

extern int sys_reboot(int cmd);

int reboot(int cmd) {
    return sys_reboot(cmd);
}

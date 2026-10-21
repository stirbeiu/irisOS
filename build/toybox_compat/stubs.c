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

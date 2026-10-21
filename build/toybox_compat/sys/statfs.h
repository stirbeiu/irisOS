#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <sys/mount.h>

#ifdef __cplusplus
extern "C" {
#endif

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STATFS_H */

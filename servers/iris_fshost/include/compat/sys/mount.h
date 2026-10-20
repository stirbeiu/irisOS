/*
 * irisOS - FreeBSD UFS2 / libufs Compatibility Header
 *
 * File: servers/iris_fshost/include/compat/sys/mount.h
 * License: BSD 2-Clause / MIT (Zone 1 Permissive)
 */

#ifndef _IRIS_COMPAT_SYS_MOUNT_H_
#define _IRIS_COMPAT_SYS_MOUNT_H_

#include <sys/cdefs.h>
#include <sys/param.h>

#ifndef __APPLE__
typedef struct fsid {
    int32_t val[2];
} fsid_t;
#else
#include_next <sys/mount.h>
#endif

struct oexport_args {
    int ex_flags;
};

#ifndef LIST_HEAD
#define LIST_HEAD(name, type) \
    struct name { struct type *lh_first; }
#endif

#ifndef LIST_ENTRY
#define LIST_ENTRY(type) \
    struct { struct type *le_next; struct type **le_prev; }
#endif

#ifndef TAILQ_HEAD
#define TAILQ_HEAD(name, type) \
    struct name { struct type *tqh_first; struct type **tqh_last; }
#endif

#ifndef TAILQ_ENTRY
#define TAILQ_ENTRY(type) \
    struct { struct type *tqe_next; struct type **tqe_prev; }
#endif

#endif /* _IRIS_COMPAT_SYS_MOUNT_H_ */

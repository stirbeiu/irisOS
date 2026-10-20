/*
 * irisOS - FreeBSD UFS2 / libufs Compatibility Header
 *
 * File: servers/iris_fshost/include/compat/sys/_sx.h
 * License: BSD 2-Clause / MIT (Zone 1 Permissive)
 */

#ifndef _IRIS_COMPAT_SYS__SX_H_
#define _IRIS_COMPAT_SYS__SX_H_

#include <sys/_lock.h>
#include <stdint.h>

struct sx {
    struct lock_object lock_object;
    volatile uintptr_t sx_lock;
};

#endif /* _IRIS_COMPAT_SYS__SX_H_ */

/*
 * irisOS - FreeBSD UFS2 / libufs Compatibility Header
 *
 * File: servers/iris_fshost/include/compat/sys/_lock.h
 * License: BSD 2-Clause / MIT (Zone 1 Permissive)
 */

#ifndef _IRIS_COMPAT_SYS__LOCK_H_
#define _IRIS_COMPAT_SYS__LOCK_H_

struct lock_object {
    const char *lo_name;
    unsigned int lo_flags;
    unsigned int lo_data;
};

#endif /* _IRIS_COMPAT_SYS__LOCK_H_ */

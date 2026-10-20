/*
 * irisOS - FreeBSD UFS2 / libufs Compatibility Header
 *
 * File: servers/iris_fshost/include/compat/sys/_task.h
 * License: BSD 2-Clause / MIT (Zone 1 Permissive)
 */

#ifndef _IRIS_COMPAT_SYS__TASK_H_
#define _IRIS_COMPAT_SYS__TASK_H_

struct task {
    int ta_pending;
};

#endif /* _IRIS_COMPAT_SYS__TASK_H_ */

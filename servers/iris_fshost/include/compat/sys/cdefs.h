/*
 * irisOS - FreeBSD UFS2 / libufs Compatibility Header
 *
 * File: servers/iris_fshost/include/compat/sys/cdefs.h
 * License: BSD 2-Clause / MIT (Zone 1 Permissive)
 */

#ifndef _IRIS_COMPAT_SYS_CDEFS_H_
#define _IRIS_COMPAT_SYS_CDEFS_H_

#ifdef __APPLE__
#include_next <sys/cdefs.h>
#endif

#include <stddef.h>
#include <stdint.h>

#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS   }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif

#ifndef __DECONST
#define __DECONST(type, var) ((type)(uintptr_t)(const void *)(var))
#endif

#ifndef __offsetof
#define __offsetof(type, field) __builtin_offsetof(type, field)
#endif

#endif /* _IRIS_COMPAT_SYS_CDEFS_H_ */

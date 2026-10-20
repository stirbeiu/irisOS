/*
 * irisOS - FreeBSD UFS2 / libufs Compatibility Header
 *
 * File: servers/iris_fshost/include/compat/sys/endian.h
 * License: BSD 2-Clause / MIT (Zone 1 Permissive)
 */

#ifndef _IRIS_COMPAT_SYS_ENDIAN_H_
#define _IRIS_COMPAT_SYS_ENDIAN_H_

#include <stdint.h>

#ifndef bswap16
#define bswap16(x) __builtin_bswap16(x)
#endif

#ifndef bswap32
#define bswap32(x) __builtin_bswap32(x)
#endif

#ifndef bswap64
#define bswap64(x) __builtin_bswap64(x)
#endif

#endif /* _IRIS_COMPAT_SYS_ENDIAN_H_ */

/*
 * irisOS - FreeBSD UFS2 / libufs Compatibility Header
 *
 * File: servers/iris_fshost/include/compat/sys/param.h
 * License: BSD 2-Clause / MIT (Zone 1 Permissive)
 */

#ifndef _IRIS_COMPAT_SYS_PARAM_H_
#define _IRIS_COMPAT_SYS_PARAM_H_

#ifdef __APPLE__
#include_next <sys/param.h>
#include <sys/time.h>
#endif

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <errno.h>

typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
typedef uint8_t        u_int8_t;
typedef uint16_t       u_int16_t;
typedef uint32_t       u_int32_t;
typedef uint64_t       u_int64_t;
typedef char*          caddr_t;
#ifndef __APPLE__
typedef int64_t        daddr_t;
#endif

#ifndef DEV_BSHIFT
#define DEV_BSHIFT 9
#endif

#ifndef DEV_BSIZE
#define DEV_BSIZE (1 << DEV_BSHIFT)
#endif

#ifndef MAXBSIZE
#define MAXBSIZE 65536
#endif

#ifndef MAXPHYS
#define MAXPHYS (128 * 1024)
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#ifndef NBBY
#define NBBY 8
#endif

#ifndef M_NOWAIT
#define M_NOWAIT 0x0001
#endif

#ifndef EINTEGRITY
#define EINTEGRITY 97
#endif

#ifndef EDOOFUS
#define EDOOFUS 88
#endif

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#undef dbtob
#define dbtob(db) ((off_t)(db) << DEV_BSHIFT)

#undef btodb
#define btodb(bytes) ((daddr_t)((uint64_t)(bytes) >> DEV_BSHIFT))

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif

#ifndef roundup2
#define roundup2(x, y) (((x) + ((y) - 1)) & (~((y) - 1)))
#endif

#ifndef powerof2
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#endif

#ifndef setbit
#define setbit(a, i) (((unsigned char *)(a))[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i) (((unsigned char *)(a))[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i)  (((const unsigned char *)(a))[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i)  ((((const unsigned char *)(a))[(i) / NBBY] & (1 << ((i) % NBBY))) == 0)
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

static inline int iris_fls(int mask) {
    if (mask == 0) return 0;
    return 32 - __builtin_clz((unsigned int)mask);
}
#ifndef fls
#define fls(x) iris_fls(x)
#endif

#ifndef bswap32
#define bswap32(x) __builtin_bswap32(x)
#endif

static inline uint32_t iris_arc4random(void) {
    static uint32_t seed = 0x49524953u;
    seed = seed * 1664525u + 1013904223u;
    return seed;
}
#ifndef __APPLE__
#ifndef arc4random
#define arc4random() iris_arc4random()
#endif
#endif

#endif /* _IRIS_COMPAT_SYS_PARAM_H_ */

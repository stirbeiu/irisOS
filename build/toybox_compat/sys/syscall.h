#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

long syscall(long number, ...);

#ifndef SYS_renameat2
#define SYS_renameat2 276
#endif

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSCALL_H */

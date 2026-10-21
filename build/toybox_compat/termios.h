#ifndef _COMPAT_TERMIOS_H
#define _COMPAT_TERMIOS_H

#include_next <termios.h>
#include <abi-bits/termios.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int cfsetspeed(struct termios *tio, speed_t speed) {
    cfsetispeed(tio, speed);
    return cfsetospeed(tio, speed);
}

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_TERMIOS_H */

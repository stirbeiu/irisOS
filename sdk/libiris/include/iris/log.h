/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/log.h
 * Purpose: Standardized uniform systemd-style logging for irisOS components.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK header.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef IRIS_LOG_H_
#define IRIS_LOG_H_

/**
 * @file log.h
 * @brief Standardized uniform systemd-style status logging.
 *
 * DESIGN & SPECIFICATION:
 *  - 4-character ANSI status tags for perfect 6-character inner bracket alignment:
 *      * OKAY: green  (\033[32m OKAY \033[0m)
 *      * WARN: yellow (\033[33m WARN \033[0m)
 *      * FAIL: red    (\033[31m FAIL \033[0m)
 *  - Output format: `[<status>] <unit> - <description>`
 *  - Atomic output: lines are buffered with snprintf before calling write(1, ...)
 *    to guarantee no split lines across multiple threads or concurrent daemons.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 4-character ANSI status tags for perfect 6-character bracket alignment */
#define STATUS_OKAY "\033[32m OKAY \033[0m"
#define STATUS_WARN "\033[33m WARN \033[0m"
#define STATUS_FAIL "\033[31m FAIL \033[0m"

#if __has_include(<stdio.h>) && __has_include(<unistd.h>)
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

/**
 * @brief Formats and writes a single atomic systemd-style status line to stdout.
 *
 * @param status Status tag macro (STATUS_OKAY, STATUS_WARN, or STATUS_FAIL).
 * @param unit Subsystem or component name (e.g. "virtio_blk", "iris_crypt", "iris_fshost").
 * @param fmt Printf-style format string describing the action or event.
 */
static inline void log_status(const char* status, const char* unit, const char* fmt, ...) {
    char desc[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(desc, sizeof(desc), fmt, ap);
    va_end(ap);

    char line_buf[512];
    int len = snprintf(line_buf, sizeof(line_buf), "[%s] %s - %s\n", status, unit, desc);
    if (len > 0) {
        size_t wlen = (size_t)len < sizeof(line_buf) ? (size_t)len : sizeof(line_buf) - 1;
        write(1, line_buf, wlen);
    }
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* IRIS_LOG_H_ */

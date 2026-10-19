/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/init.h
 * Purpose: Runtime initialization and self capability accessors for irisOS processes.
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

#ifndef IRIS_INIT_H_
#define IRIS_INIT_H_

/**
 * @file init.h
 * @brief Process self-capability accessors and runtime initialization for irisOS.
 */

#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize runtime self capability handles for the current process.
 *
 * @param proc Handle to own process (PA_PROC_SELF).
 * @param vmar Handle to root VMAR (PA_VMAR_ROOT).
 * @param job Handle to default job (PA_JOB_DEFAULT).
 */
void iris_init_runtime(zx_handle_t proc, zx_handle_t vmar, zx_handle_t job);

/**
 * @brief Retrieve handle to the current process.
 * @return Process capability handle, or ZX_HANDLE_INVALID if uninitialized.
 */
zx_handle_t iris_process_self(void);

/**
 * @brief Retrieve handle to the root VMAR of the current process.
 * @return Root VMAR capability handle, or ZX_HANDLE_INVALID if uninitialized.
 */
zx_handle_t iris_vmar_root_self(void);

/**
 * @brief Retrieve handle to the default parent job for creating child processes.
 * @return Job capability handle, or ZX_HANDLE_INVALID if uninitialized.
 */
zx_handle_t iris_job_default(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_INIT_H_ */

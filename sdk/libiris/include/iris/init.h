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
 *
 * ARCHITECTURAL DESIGN & CAPABILITY MODEL:
 *  Under Zircon's capability model, a process does not possess ambient authority.
 *  Privileges such as virtual address space manipulation (VMAR), child process
 *  creation (Job), and self-inspection (Process) require explicit capability handles.
 *
 *  During bootstrap, the kernel / parent process injects these handles into the
 *  process's bootstrap channel via `<zircon/processargs.h>` tags:
 *   - `PA_HND(PA_PROC_SELF, 0)`:   Handle to the process itself.
 *   - `PA_HND(PA_VMAR_ROOT, 0)`:   Handle to the process's root address space.
 *   - `PA_HND(PA_JOB_DEFAULT, 0)`: Handle to the default parent job.
 *
 *  `libiris` caches these handles internally via `iris_init_runtime()` so that
 *  user-space code and libraries can conveniently access self-capabilities
 *  without maintaining repetitive global variables.
 *
 * CAPABILITY LIFECYCLE:
 *  - Handles supplied to `iris_init_runtime()` are BORROWED; ownership of the
 *    underlying descriptor remains with the process until termination.
 *  - Handles returned by `iris_process_self()`, `iris_vmar_root_self()`, and
 *    `iris_job_default()` are borrowed references and MUST NOT be closed by callers.
 */

#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize runtime self capability handles for the current process.
 *
 * Called during C runtime startup (`_start` / `__iris_entry`) or early server
 * initialization to store the process's primary capability handles.
 *
 * @param proc Handle to own process (`PA_PROC_SELF`). Requires `ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_DUPLICATE`.
 * @param vmar Handle to root VMAR (`PA_VMAR_ROOT`). Requires `ZX_RIGHT_MAP | ZX_RIGHT_DUPLICATE`.
 * @param job  Handle to default job (`PA_JOB_DEFAULT`). Requires `ZX_RIGHT_MANAGE_PROCESS | ZX_RIGHT_ENUMERATE`.
 *
 * @note If any handle is `ZX_HANDLE_INVALID`, the respective subsystem queries
 *       will return `ZX_HANDLE_INVALID` or fall back to system defaults.
 */
void iris_init_runtime(zx_handle_t proc, zx_handle_t vmar, zx_handle_t job);

/**
 * @brief Retrieve handle to the current process.
 *
 * Used for process-level operations such as `zx_object_get_info()`,
 * `zx_object_set_property()`, or creating child threads via `zx_thread_create()`.
 *
 * @return Process capability handle, or `ZX_HANDLE_INVALID` if uninitialized.
 * @warning Do NOT close the returned handle via `zx_handle_close()`.
 */
zx_handle_t iris_process_self(void);

/**
 * @brief Retrieve handle to the root VMAR of the current process.
 *
 * Used for virtual address space allocations via `zx_vmar_map()`,
 * sub-region reservations via `zx_vmar_allocate()`, and unmapping via `zx_vmar_unmap()`.
 *
 * @return Root VMAR capability handle, or `ZX_HANDLE_INVALID` if uninitialized.
 * @warning Do NOT close the returned handle via `zx_handle_close()`.
 */
zx_handle_t iris_vmar_root_self(void);

/**
 * @brief Retrieve handle to the default parent job for creating child processes.
 *
 * Used by process spawning routines (`iris_spawn`) to attach newly created
 * child processes to the current job hierarchy.
 *
 * @return Job capability handle, or `ZX_HANDLE_INVALID` if uninitialized.
 * @warning Do NOT close the returned handle via `zx_handle_close()`.
 */
zx_handle_t iris_job_default(void);

/**
 * @brief Store the Root Resource handle for the current driver/server process.
 *
 * @param rsrc Root Resource capability handle (`PA_RESOURCE`).
 */
void iris_set_root_resource(zx_handle_t rsrc);

/**
 * @brief Retrieve handle to the root resource if passed to the current process.
 *
 * @return Root Resource handle, or ZX_HANDLE_INVALID if not available.
 */
zx_handle_t iris_root_resource(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_INIT_H_ */

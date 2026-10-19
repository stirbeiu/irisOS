/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/spawn.h
 * Purpose: User-space process spawning primitives wrapping zx_process_create/start and zx_proc_args_t.
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

#ifndef IRIS_SPAWN_H_
#define IRIS_SPAWN_H_

/**
 * @file spawn.h
 * @brief Process creation and bootstrap ABI primitives for irisOS.
 *
 * ARCHITECTURAL DESIGN & PROCESS MODEL:
 *  Per Section 4.6 of AGENTS.md:
 *   - Zircon microkernel does not support copy-on-write `fork()`. Process creation
 *     in irisOS is modeled cleanly on POSIX `posix_spawn()` implemented over
 *     `zx_process_create()`, `zx_thread_create()`, and `zx_process_start()`.
 *   - State transfer to the child process (arguments, environment, open file descriptors,
 *     and capability handles) is conducted via the standard `<zircon/processargs.h>`
 *     bootstrap channel protocol (`zx_proc_args_t`).
 *   - File descriptors are transferred using standard `PA_HND(PA_FD, fd)` handle info tags,
 *     enabling parent processes to wire `stdin` (fd 0), `stdout` (fd 1), `stderr` (fd 2),
 *     or arbitrary IPC channels directly to child processes.
 *   - When `vmo_executable` is provided, `iris_spawn()` parses the 64-bit ELF image,
 *     maps all `PT_LOAD` segments into the child's address space with proper permissions
 *     (`ZX_VM_PERM_READ`, `ZX_VM_PERM_WRITE`, `ZX_VM_PERM_EXECUTE`), and calculates
 *     the entry point address automatically.
 *
 * CAPABILITY REQUIREMENTS:
 *  - `job`: Requires `ZX_RIGHT_MANAGE_PROCESS | ZX_RIGHT_ENUMERATE`.
 *  - `vmo_executable`: Requires `ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_EXECUTE`.
 *  - `vmo_vdso`: Requires `ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_EXECUTE | ZX_RIGHT_DUPLICATE`.
 *  - Action handles: Require `ZX_RIGHT_TRANSFER`.
 *
 * CAPABILITY LIFECYCLE:
 *  - Handles supplied in `args->actions` marked for transfer are CONSUMED on `ZX_OK`
 *    and transferred into the child process's bootstrap channel. On failure, ownership
 *    remains with the caller.
 *  - Handles returned via `out_process` and `out_thread` are ACQUIRED by the caller;
 *    the caller must close them with `zx_handle_close()`.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zircon/types.h>
#include <zircon/processargs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle type identifier for file descriptor inheritance: PA_HND(PA_FD, target_fd).
 *
 * Defined in Section 4.6 of AGENTS.md for POSIX compatibility over Zircon channels.
 */
#ifndef PA_FD
#define PA_FD 0x30u
#endif

/**
 * @brief Default initial stack size for spawned processes (256 KB).
 */
#define IRIS_SPAWN_DEFAULT_STACK_SIZE (256u * 1024u)

/**
 * @brief Maximum actions / file descriptors transferable in a single spawn transaction.
 */
#define IRIS_SPAWN_MAX_ACTIONS 64u

/**
 * @brief Types of handle transfer actions performed during process spawning.
 */
typedef enum iris_spawn_action_type {
    IRIS_SPAWN_ACTION_TRANSFER_HANDLE = 1, /**< Transfer generic capability handle with given PA_HND info tag */
    IRIS_SPAWN_ACTION_ADD_FD          = 2, /**< Transfer channel handle as file descriptor: PA_HND(PA_FD, target_fd) */
} iris_spawn_action_type_t;

/**
 * @brief Specification of a single capability handle or file descriptor to transfer to a child.
 */
typedef struct iris_spawn_action {
    iris_spawn_action_type_t type;        /**< Action type (TRANSFER_HANDLE or ADD_FD) */
    zx_handle_t              handle;      /**< Handle to transfer (consumed on spawn success) */
    uint32_t                 handle_info; /**< Handle info tag: PA_HND(type, arg) if type == TRANSFER_HANDLE */
    int                      target_fd;   /**< Target file descriptor number (0, 1, 2...) if type == ADD_FD */
} iris_spawn_action_t;

/**
 * @brief Process creation parameters passed to `iris_spawn()`.
 */
typedef struct iris_spawn_args {
    zx_handle_t                job;            /**< Parent Job handle (uses zx_job_default() if invalid) */
    const char*                name;           /**< Human-readable process name (e.g. "iris_fshost", "hello_c") */
    zx_handle_t                vmo_executable; /**< Optional ELF executable VMO. If valid, ELF segments are parsed and mapped */
    zx_vaddr_t                 entry;          /**< Entry point address. Required if vmo_executable is invalid; ignored otherwise */
    size_t                     stack_size;     /**< Initial stack size in bytes (0 for 256 KB default) */
    zx_handle_t                vmo_vdso;       /**< vDSO VMO handle (mapped into child and passed via PA_VMO_VDSO) */
    zx_vaddr_t                 vdso_base;      /**< Pre-mapped vDSO base address in child, or 0 to map automatically */

    const char* const*         argv;           /**< Array of null-terminated command line arguments */
    uint32_t                   argc;           /**< Count of argument strings in argv */

    const char* const*         envp;           /**< Array of null-terminated "NAME=VALUE" environment strings */
    uint32_t                   envc;           /**< Count of environment strings in envp */

    const iris_spawn_action_t* actions;        /**< Array of capability handle and FD transfer actions */
    size_t                     num_actions;    /**< Count of actions in `actions` */
} iris_spawn_args_t;

/**
 * @brief Spawn a new user-space process and launch its initial thread.
 *
 * Implements the full Zircon process creation pipeline:
 *  1. Creates child process and root VMAR in target Job via `zx_process_create()`.
 *  2. Creates initial thread via `zx_thread_create()`.
 *  3. If `vmo_executable` is valid, parses 64-bit ELF headers, maps `PT_LOAD` segments into
 *     child VMAR, and computes the runtime entry point.
 *  4. Allocates and maps initial stack VMO into child VMAR; aligns stack pointer.
 *  5. Maps vDSO into child VMAR if `vmo_vdso` is provided.
 *  6. Assembles `zx_proc_args_t` message containing arguments, environment, and handle table
 *     including core handles (`PA_PROC_SELF`, `PA_THREAD_SELF`, `PA_VMAR_ROOT`, `PA_VMO_STACK`,
 *     `PA_VMO_VDSO`, `PA_JOB_DEFAULT`) and custom descriptors (`PA_HND(PA_FD, target_fd)`).
 *  7. Writes message and transfers handles across bootstrap channel.
 *  8. Executes `zx_process_start()` to begin child execution.
 *
 * @param args Complete process creation specification.
 * @param[out] out_process Returns capability handle to the spawned process.
 * @param[out] out_thread Optional pointer to receive initial thread handle (may be NULL).
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_INVALID_ARGS if `args` or `out_process` is NULL or `args->name` is missing.
 * @return ZX_ERR_NO_MEMORY if system memory is insufficient.
 * @return ZX_ERR_BAD_HANDLE if `job` or an action handle is invalid.
 * @return ZX_ERR_NOT_FOUND or ZX_ERR_BAD_STATE if ELF parsing failed.
 *
 * @note Capability Lifecycle: On success, handles in `args->actions` are consumed, and
 *       the caller acquires ownership of `*out_process` (and `*out_thread` if requested).
 */
zx_status_t iris_spawn(const iris_spawn_args_t* args,
                       zx_handle_t* out_process,
                       zx_handle_t* out_thread);

/**
 * @brief High-level helper: spawn an ELF binary directly from an executable VMO.
 *
 * @param job Parent job handle (pass ZX_HANDLE_INVALID for default job).
 * @param name Process name.
 * @param vmo_elf Executable VMO containing 64-bit ELF binary.
 * @param vmo_vdso vDSO VMO handle for system call dispatch.
 * @param argv Command line arguments (may be NULL if argc == 0).
 * @param argc Count of arguments.
 * @param actions Handle transfer actions (may be NULL if num_actions == 0).
 * @param num_actions Count of actions.
 * @param[out] out_process Returns child process capability handle.
 * @param[out] out_thread Optional pointer to receive initial thread handle.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_spawn_elf(zx_handle_t job,
                           const char* name,
                           zx_handle_t vmo_elf,
                           zx_handle_t vmo_vdso,
                           const char* const* argv,
                           uint32_t argc,
                           const iris_spawn_action_t* actions,
                           size_t num_actions,
                           zx_handle_t* out_process,
                           zx_handle_t* out_thread);

/**
 * @brief Convenience helper: spawn a minimal process with pre-computed entry point and vDSO.
 *
 * @param job Parent job handle (pass `ZX_HANDLE_INVALID` to use default job).
 * @param name Process name.
 * @param entry Virtual address of entry point in child.
 * @param vmo_vdso vDSO VMO handle (pass `ZX_HANDLE_INVALID` if none).
 * @param[out] out_process Returns process capability handle.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_spawn_simple(zx_handle_t job,
                              const char* name,
                              zx_vaddr_t entry,
                              zx_handle_t vmo_vdso,
                              zx_handle_t* out_process);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_SPAWN_H_ */

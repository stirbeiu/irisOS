/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/include/iris/fd_table.h
 * Purpose: Client-side file descriptor table mapping int fd -> Zircon channel/handle.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK / sysdeps header.
 */

#ifndef _IRIS_FD_TABLE_H_
#define _IRIS_FD_TABLE_H_

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum file descriptors tracked per process.
 */
#define IRIS_MAX_FD 256

/**
 * @brief Underlying descriptor type for each open file descriptor table entry.
 */
typedef enum iris_fd_type {
    IRIS_FD_CLOSED   = 0, /**< Slot is unused / closed */
    IRIS_FD_DEBUGLOG = 1, /**< Kernel debuglog handle (early console write-only) */
    IRIS_FD_CHANNEL  = 2, /**< IPC channel to server (iris_fshost, iris_pty, etc.) */
} iris_fd_type_t;

/**
 * @brief Representation of an entry in the client-side file descriptor table.
 */
typedef struct iris_fd_entry {
    zx_handle_t    handle; /**< Zircon capability handle (channel or debuglog) */
    int            flags;  /**< POSIX file status flags (O_RDONLY, O_WRONLY, O_RDWR, etc.) */
    off_t          offset; /**< Current file seek position */
    iris_fd_type_t type;   /**< Descriptor category */
} iris_fd_entry_t;

/**
 * @brief Initialize the global client-side file descriptor table.
 *
 * Default configuration:
 *  - FDs 0, 1, and 2 default to IRIS_FD_DEBUGLOG routing to the kernel debug log
 *    until explicitly rebound to channels via startup handles or open().
 */
void iris_fd_table_init(void);

/**
 * @brief Bind an explicit file descriptor number to a Zircon handle.
 *
 * @param fd Target file descriptor (0 <= fd < IRIS_MAX_FD).
 * @param handle Capability handle to bind.
 * @param type Descriptor type (IRIS_FD_CHANNEL or IRIS_FD_DEBUGLOG).
 * @param flags POSIX open flags.
 * @return 0 on success, or -1 on error (e.g. fd out of range).
 */
int iris_fd_bind(int fd, zx_handle_t handle, iris_fd_type_t type, int flags);

/**
 * @brief Allocate the next lowest available file descriptor (>= 3) and bind a handle.
 *
 * @param handle Capability handle to bind.
 * @param type Descriptor type.
 * @param flags POSIX open flags.
 * @return Allocated file descriptor number, or -1 if the table is full (EMFILE).
 */
int iris_fd_alloc(zx_handle_t handle, iris_fd_type_t type, int flags);

/**
 * @brief Look up an entry in the file descriptor table.
 *
 * @param fd File descriptor number.
 * @return Pointer to entry, or NULL if fd is invalid.
 */
iris_fd_entry_t* iris_fd_get(int fd);

/**
 * @brief Get the capability handle bound to a file descriptor.
 *
 * @param fd File descriptor number.
 * @return Capability handle, or ZX_HANDLE_INVALID if fd is closed or invalid.
 */
zx_handle_t iris_fd_get_handle(int fd);

/**
 * @brief Close an open file descriptor, releasing its bound handle if applicable.
 *
 * @param fd File descriptor number to close.
 * @return 0 on success, or -1 on error (EBADF).
 */
int iris_fd_close(int fd);

/**
 * @brief Count currently open file descriptors.
 */
int iris_fd_count_open(void);

/**
 * @brief Update the global debuglog handle and synchronizes default stdio descriptors.
 */
void iris_set_debuglog(zx_handle_t handle);

/**
 * @brief Store the inherited vDSO VMO handle for child process spawning.
 */
void iris_set_vdso_vmo(zx_handle_t handle);

/**
 * @brief Store the inherited BOOTFS VMO handle for executable loading.
 */
void iris_set_bootfs_vmo(zx_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* _IRIS_FD_TABLE_H_ */

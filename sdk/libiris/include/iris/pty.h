/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/pty.h
 * Purpose: Client-side C API helpers for Pseudo-Terminal (iris_pty) operations.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK header.
 */

#ifndef IRIS_PTY_H_
#define IRIS_PTY_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zircon/types.h>
#include <iris/ipc.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure terminal line discipline mode (canonical/raw, echo).
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param flags Bitwise OR of IRIS_PTY_MODE_* flags.
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_set_mode(zx_handle_t pty_ch, uint32_t flags);

/**
 * @brief Query current terminal line discipline mode flags.
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param out_flags Pointer to uint32_t receiving mode flags.
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_get_mode(zx_handle_t pty_ch, uint32_t* out_flags);

/**
 * @brief Set terminal window dimensions (rows and columns).
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param rows Terminal row count.
 * @param cols Terminal column count.
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_set_window(zx_handle_t pty_ch, uint16_t rows, uint16_t cols);

/**
 * @brief Query terminal window dimensions.
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param out_rows Pointer receiving row count.
 * @param out_cols Pointer receiving column count.
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_get_window(zx_handle_t pty_ch, uint16_t* out_rows, uint16_t* out_cols);

/**
 * @brief Inject raw or synthetic input bytes into PTY input stream.
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param data Buffer of characters/bytes to inject.
 * @param len Number of bytes to inject.
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_inject(zx_handle_t pty_ch, const void* data, size_t len);

/**
 * @brief Query pending POSIX signal (e.g. SIGINT from Ctrl+C) from PTY server.
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param out_signo Pointer receiving signal number (0 if none pending).
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_get_signal(zx_handle_t pty_ch, uint32_t* out_signo);

/**
 * @brief Read character stream from PTY via IPC.
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param buf Destination buffer.
 * @param max_len Maximum bytes to read.
 * @param out_read Actual bytes returned.
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_read(zx_handle_t pty_ch, void* buf, size_t max_len, size_t* out_read);

/**
 * @brief Write character stream to PTY console via IPC.
 *
 * @param pty_ch Channel handle to iris_pty server.
 * @param buf Data bytes to write.
 * @param len Number of bytes to write.
 * @return ZX_OK on success, or negative error code.
 */
zx_status_t iris_pty_write(zx_handle_t pty_ch, const void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_PTY_H_ */

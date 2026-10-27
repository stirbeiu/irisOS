/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_pty/include/pty.h
 * Purpose: Internal header for the Ring 3 Pseudo-Terminal Server Daemon (iris_pty).
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 server header.
 */

#ifndef SERVERS_IRIS_PTY_H_
#define SERVERS_IRIS_PTY_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zircon/types.h>
#include <iris/ipc.h>
#include <servers/drivers/virtio_input/include/virtio_input.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTY_LINE_BUF_MAX   4096
#define PTY_READY_BUF_MAX  16384
#define PTY_MAX_CLIENTS    32
#define PTY_HIST_MAX       16
#define PTY_HIST_LINE_MAX  256

/**
 * @brief Terminal line discipline state.
 */
typedef struct pty_discipline {
    uint32_t          mode_flags;
    iris_pty_window_t window;
    uint32_t          pending_signal;
    bool              eof_pending;

    /* Canonical editing line buffer & cursor position */
    char              line_buf[PTY_LINE_BUF_MAX];
    size_t            line_len;
    size_t            line_cursor;

    /* VT100 / ANSI escape sequence parser state for canonical editing */
    uint8_t           esc_state;
    char              esc_param;

    /* Command history ring for Up/Down arrow keys in canonical mode */
    char              history[PTY_HIST_MAX][PTY_HIST_LINE_MAX];
    size_t            hist_count;
    size_t            hist_view;
    char              saved_line[PTY_HIST_LINE_MAX];
    size_t            saved_len;

    /* Ready queue (characters ready for read()) */
    char              ready_buf[PTY_READY_BUF_MAX];
    size_t            ready_head;
    size_t            ready_tail;
    size_t            ready_count;

    /* Spinlock protecting discipline buffers */
    int               lock;
} pty_discipline_t;

/**
 * @brief Feed a character into the PTY line discipline.
 */
void pty_feed_char(pty_discipline_t* pty, char c);

/**
 * @brief Output character stream to console / debuglog.
 */
void pty_raw_output(const char* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SERVERS_IRIS_PTY_H_ */

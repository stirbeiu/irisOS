/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/src/pty.c
 * Purpose: Client-side C API implementation for Pseudo-Terminal (iris_pty) operations.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK source.
 */

#include <iris/pty.h>
#include <iris/channel.h>

static void* iris_memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static uint32_t s_pty_txid = 500;

zx_status_t iris_pty_set_mode(zx_handle_t pty_ch, uint32_t flags) {
    if (pty_ch == ZX_HANDLE_INVALID) return ZX_ERR_BAD_HANDLE;

    iris_pty_mode_t req = { flags };
    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_SET_MODE, ++s_pty_txid, sizeof(req));

    iris_msg_hdr_t resp_hdr;
    return iris_channel_call(pty_ch, &req_hdr, &req, NULL, 0,
                             &resp_hdr, NULL, 0, NULL,
                             NULL, 0, NULL, ZX_TIME_INFINITE);
}

zx_status_t iris_pty_get_mode(zx_handle_t pty_ch, uint32_t* out_flags) {
    if (pty_ch == ZX_HANDLE_INVALID || !out_flags) return ZX_ERR_INVALID_ARGS;

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_GET_MODE, ++s_pty_txid, 0);

    iris_msg_hdr_t resp_hdr;
    iris_pty_mode_t resp;
    uint32_t act_len = 0;
    zx_status_t st = iris_channel_call(pty_ch, &req_hdr, NULL, NULL, 0,
                                       &resp_hdr, &resp, sizeof(resp), &act_len,
                                       NULL, 0, NULL, ZX_TIME_INFINITE);
    if (st == ZX_OK) {
        *out_flags = resp.flags;
    }
    return st;
}

zx_status_t iris_pty_set_window(zx_handle_t pty_ch, uint16_t rows, uint16_t cols) {
    if (pty_ch == ZX_HANDLE_INVALID) return ZX_ERR_BAD_HANDLE;

    iris_pty_window_t win = { rows, cols, 0, 0 };
    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_SET_WINDOW, ++s_pty_txid, sizeof(win));

    iris_msg_hdr_t resp_hdr;
    return iris_channel_call(pty_ch, &req_hdr, &win, NULL, 0,
                             &resp_hdr, NULL, 0, NULL,
                             NULL, 0, NULL, ZX_TIME_INFINITE);
}

zx_status_t iris_pty_get_window(zx_handle_t pty_ch, uint16_t* out_rows, uint16_t* out_cols) {
    if (pty_ch == ZX_HANDLE_INVALID || !out_rows || !out_cols) return ZX_ERR_INVALID_ARGS;

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_GET_WINDOW, ++s_pty_txid, 0);

    iris_msg_hdr_t resp_hdr;
    iris_pty_window_t resp;
    uint32_t act_len = 0;
    zx_status_t st = iris_channel_call(pty_ch, &req_hdr, NULL, NULL, 0,
                                       &resp_hdr, &resp, sizeof(resp), &act_len,
                                       NULL, 0, NULL, ZX_TIME_INFINITE);
    if (st == ZX_OK) {
        *out_rows = resp.rows;
        *out_cols = resp.cols;
    }
    return st;
}

zx_status_t iris_pty_inject(zx_handle_t pty_ch, const void* data, size_t len) {
    if (pty_ch == ZX_HANDLE_INVALID || !data || len == 0) return ZX_ERR_INVALID_ARGS;
    if (len > 4096) return ZX_ERR_OUT_OF_RANGE;

    uint8_t payload[sizeof(iris_pty_inject_req_t) + 4096];
    iris_pty_inject_req_t* req = (iris_pty_inject_req_t*)payload;
    req->count = (uint32_t)len;
    iris_memcpy(payload + sizeof(iris_pty_inject_req_t), data, len);

    uint32_t req_size = (uint32_t)(sizeof(iris_pty_inject_req_t) + len);
    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_INJECT, ++s_pty_txid, req_size);

    iris_msg_hdr_t resp_hdr;
    iris_pty_inject_resp_t resp;
    uint32_t act_len = 0;
    return iris_channel_call(pty_ch, &req_hdr, payload, NULL, 0,
                             &resp_hdr, &resp, sizeof(resp), &act_len,
                             NULL, 0, NULL, ZX_TIME_INFINITE);
}

zx_status_t iris_pty_get_signal(zx_handle_t pty_ch, uint32_t* out_signo) {
    if (pty_ch == ZX_HANDLE_INVALID || !out_signo) return ZX_ERR_INVALID_ARGS;

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_SIGNAL, ++s_pty_txid, 0);

    iris_msg_hdr_t resp_hdr;
    iris_pty_signal_req_t resp;
    uint32_t act_len = 0;
    zx_status_t st = iris_channel_call(pty_ch, &req_hdr, NULL, NULL, 0,
                                       &resp_hdr, &resp, sizeof(resp), &act_len,
                                       NULL, 0, NULL, ZX_TIME_INFINITE);
    if (st == ZX_OK) {
        *out_signo = resp.signo;
    }
    return st;
}

zx_status_t iris_pty_read(zx_handle_t pty_ch, void* buf, size_t max_len, size_t* out_read) {
    if (pty_ch == ZX_HANDLE_INVALID || !buf || max_len == 0) return ZX_ERR_INVALID_ARGS;

    iris_pty_read_req_t req = { (uint32_t)max_len };
    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_READ, ++s_pty_txid, sizeof(req));

    iris_msg_hdr_t resp_hdr;
    uint8_t resp_buf[sizeof(iris_pty_read_resp_t) + 4096];
    uint32_t act_len = 0;
    zx_status_t st = iris_channel_call(pty_ch, &req_hdr, &req, NULL, 0,
                                       &resp_hdr, resp_buf, sizeof(resp_buf), &act_len,
                                       NULL, 0, NULL, ZX_TIME_INFINITE);
    if (st != ZX_OK) return st;

    if (act_len >= sizeof(iris_pty_read_resp_t)) {
        iris_pty_read_resp_t* r = (iris_pty_read_resp_t*)resp_buf;
        uint32_t bytes = r->bytes_read;
        if (bytes > 0 && act_len >= sizeof(iris_pty_read_resp_t) + bytes) {
            uint32_t copy_cnt = (bytes < max_len) ? bytes : (uint32_t)max_len;
            iris_memcpy(buf, resp_buf + sizeof(iris_pty_read_resp_t), copy_cnt);
            if (out_read) *out_read = copy_cnt;
            return ZX_OK;
        } else if (bytes == 0) {
            /* EOF */
            if (out_read) *out_read = 0;
            return ZX_OK;
        }
    }
    if (out_read) *out_read = 0;
    return ZX_OK;
}

zx_status_t iris_pty_write(zx_handle_t pty_ch, const void* buf, size_t len) {
    if (pty_ch == ZX_HANDLE_INVALID || !buf || len == 0) return ZX_ERR_INVALID_ARGS;
    if (len > 4096) len = 4096;

    uint8_t payload[sizeof(iris_pty_write_req_t) + 4096];
    iris_pty_write_req_t* req = (iris_pty_write_req_t*)payload;
    req->count = (uint32_t)len;
    iris_memcpy(payload + sizeof(iris_pty_write_req_t), buf, len);

    uint32_t req_size = (uint32_t)(sizeof(iris_pty_write_req_t) + len);
    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_PTY, IRIS_PTY_OP_WRITE, ++s_pty_txid, req_size);

    iris_msg_hdr_t resp_hdr;
    iris_pty_write_resp_t resp;
    uint32_t act_len = 0;
    return iris_channel_call(pty_ch, &req_hdr, payload, NULL, 0,
                             &resp_hdr, &resp, sizeof(resp), &act_len,
                             NULL, 0, NULL, ZX_TIME_INFINITE);
}

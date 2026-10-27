/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: apps/sysutils/passphrase/main.c
 * Purpose: Interactive disk passphrase unlock utility (`bin/passphrase`).
 *
 * Lifecycle & Capabilities:
 *  - Spawned by `iris_init` on `iris_pty` during interactive boot prior to mounting
 *    `/Users` in `iris_fshost`.
 *  - Prompts `Enter disk passphrase: ` to stdout (FD 1).
 *  - Disables terminal echoing and canonical line buffering using `tcsetattr()`
 *    (raw mode), reads the user's passphrase character-by-character until Enter,
 *    and restores original terminal attributes.
 *  - Sends `IRIS_CRYPT_OP_UNLOCK` over `IRIS_PROTO_CRYPT` (`PA_HND(PA_USER2, 0)`)
 *    to `iris_crypt` to derive the 512-bit AES-256-XTS master key via Argon2id.
 *
 * Licensing: MIT License. Permissive Zone 1 / System Utility.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iris/ipc.h>
#include <iris/log.h>

#define MAX_PASSPHRASE_ATTEMPTS 3

static zx_status_t send_unlock_request(zx_handle_t crypt_ch, const char* pw, size_t pw_len) {
    if (crypt_ch == ZX_HANDLE_INVALID) {
        return ZX_ERR_BAD_HANDLE;
    }

    static uint32_t s_txid = 100;
    uint8_t msg[sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_unlock_req_t)] __attribute__((aligned(8)));
    memset(msg, 0, sizeof(msg));

    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)msg;
    iris_crypt_unlock_req_t* req = (iris_crypt_unlock_req_t*)(msg + sizeof(iris_msg_hdr_t));

    if (pw_len >= sizeof(req->passphrase)) {
        pw_len = sizeof(req->passphrase) - 1;
    }
    req->passphrase_len = (uint32_t)pw_len;
    if (pw_len > 0 && pw != NULL) {
        memcpy(req->passphrase, pw, pw_len);
        req->passphrase[pw_len] = '\0';
    }

    iris_msg_hdr_init(hdr, IRIS_PROTO_CRYPT, IRIS_CRYPT_OP_UNLOCK, ++s_txid, sizeof(iris_crypt_unlock_req_t));

    zx_status_t st = zx_channel_write(crypt_ch, 0, msg, sizeof(msg), NULL, 0);
    /* Zeroize sensitive stack buffer immediately */
    memset(req, 0, sizeof(*req));
    if (st != ZX_OK) {
        return st;
    }

    zx_signals_t obs = 0;
    st = zx_object_wait_one(crypt_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) {
        return ZX_ERR_PEER_CLOSED;
    }

    iris_msg_hdr_t rep;
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(crypt_ch, 0, &rep, NULL, sizeof(rep), 0, &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t)) {
        return ZX_ERR_IO;
    }

    return rep.status;
}

static size_t read_passphrase_noecho(char* out_buf, size_t max_len) {
    struct termios orig_term, raw_term;
    bool term_saved = (tcgetattr(0, &orig_term) == 0);
    if (term_saved) {
        raw_term = orig_term;
        raw_term.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHOE | ECHOK);
        tcsetattr(0, TCSANOW, &raw_term);
    }

    size_t len = 0;
    uint8_t esc_state = 0;
    for (;;) {
        char c = 0;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) {
            break;
        }
        /* Filter out VT100 escape sequences (e.g. arrow keys) while typing passphrase */
        if (esc_state == 1) {
            if (c == '[' || c == 'O') {
                esc_state = 2;
                continue;
            }
            esc_state = 0;
        } else if (esc_state == 2) {
            if (c >= '0' && c <= '9') {
                esc_state = 3;
                continue;
            }
            esc_state = 0;
            continue;
        } else if (esc_state == 3) {
            esc_state = 0;
            continue;
        }
        if ((uint8_t)c == 0x1B) {
            esc_state = 1;
            continue;
        }

        if (c == '\r' || c == '\n') {
            break;
        }
        /* Handle Ctrl+D (EOF) or Ctrl+C */
        if (c == 0x04 || c == 0x03) {
            break;
        }
        /* Handle Backspace (0x08 or 0x7F) */
        if (c == '\b' || (uint8_t)c == 0x08 || (uint8_t)c == 0x7F) {
            if (len > 0) {
                out_buf[--len] = '\0';
            }
            continue;
        }
        if ((uint8_t)c >= 0x20 && len + 1 < max_len) {
            out_buf[len++] = c;
        }
    }
    out_buf[len] = '\0';

    if (term_saved) {
        tcsetattr(0, TCSANOW, &orig_term);
    }
    const char* nl = "\r\n";
    write(1, nl, 2);
    return len;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    zx_handle_t crypt_ch = zx_take_startup_handle(PA_HND(PA_USER2, 0));
    if (crypt_ch == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "passphrase", "Missing iris_crypt IPC channel (PA_USER2).");
        return 1;
    }

    char pw_buf[256];
    for (int attempt = 1; attempt <= MAX_PASSPHRASE_ATTEMPTS; ++attempt) {
        const char* prompt = "Enter disk passphrase: ";
        write(1, prompt, strlen(prompt));

        memset(pw_buf, 0, sizeof(pw_buf));
        size_t pw_len = read_passphrase_noecho(pw_buf, sizeof(pw_buf));

        zx_status_t st = send_unlock_request(crypt_ch, pw_buf, pw_len);
        /* Wipe passphrase buffer from RAM immediately */
        memset(pw_buf, 0, sizeof(pw_buf));

        if (st == ZX_OK) {
            log_status(STATUS_OKAY, "passphrase", "Unlocked Data partition via Argon2id + AES-256-XTS.");
            zx_handle_close(crypt_ch);
            return 0;
        }

        log_status(STATUS_WARN, "passphrase", "Invalid disk passphrase (attempt %d/%d).",
                   attempt, MAX_PASSPHRASE_ATTEMPTS);
    }

    log_status(STATUS_FAIL, "passphrase", "Maximum passphrase attempts exceeded.");
    zx_handle_close(crypt_ch);
    return 1;
}

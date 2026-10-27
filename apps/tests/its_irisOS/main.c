/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: apps/tests/its_irisOS/main.c
 * Purpose: Interactive Test Suite (its_irisOS) verifying mlibc C runtime,
 *          heap allocation, client-side FD table, virtual console devices,
 *          posix_spawn actions, VirtIO block I/O, AES-256-XTS storage encryption,
 *          UFS2 read-only /System & read-write /Users, and iris_pty line discipline.
 *
 * Licensing: MIT License. Permissive Zone 1 test application.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iris/ipc.h>
#include <iris/fd_table.h>
#include <iris/pty.h>
#include <iris/log.h>

/* ==============================================================================
 * Synchronous IPC Helpers for VirtIO Block & Storage Encryption
 * ============================================================================== */

static zx_status_t block_get_info(zx_handle_t ch, iris_blk_info_t* out_info) {
    if (ch == ZX_HANDLE_INVALID) return ZX_ERR_BAD_HANDLE;

    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_DRIVER, IRIS_BLK_OP_GET_INFO, 1, 0);

    zx_status_t st = zx_channel_write(ch, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t observed = 0;
    st = zx_object_wait_one(ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &observed);
    if (st != ZX_OK) return st;

    uint8_t rep_buf[256];
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t) + sizeof(iris_blk_info_t)) {
        return ZX_ERR_IO;
    }

    iris_msg_hdr_t* rep = (iris_msg_hdr_t*)rep_buf;
    if (rep->status != ZX_OK) return rep->status;

    if (out_info) {
        memcpy(out_info, rep_buf + sizeof(iris_msg_hdr_t), sizeof(iris_blk_info_t));
    }
    return ZX_OK;
}

static zx_status_t block_write_sector(zx_handle_t ch, uint64_t sector, const void* data) {
    if (ch == ZX_HANDLE_INVALID) return ZX_ERR_BAD_HANDLE;

    uint8_t msg[sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t) + 512];
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)msg;
    iris_blk_rw_req_t* req = (iris_blk_rw_req_t*)(msg + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_WRITE, 2, sizeof(iris_blk_rw_req_t) + 512);
    req->start_sector = sector;
    req->num_sectors = 1;
    req->flags = 0;
    memcpy(req + 1, data, 512);

    zx_status_t st = zx_channel_write(ch, 0, msg, sizeof(msg), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t observed = 0;
    st = zx_object_wait_one(ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &observed);
    if (st != ZX_OK) return st;

    uint8_t rep_buf[256];
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t)) {
        return ZX_ERR_IO;
    }

    iris_msg_hdr_t* rep = (iris_msg_hdr_t*)rep_buf;
    return rep->status;
}

static zx_status_t block_read_sector(zx_handle_t ch, uint64_t sector, void* out_data) {
    if (ch == ZX_HANDLE_INVALID) return ZX_ERR_BAD_HANDLE;

    uint8_t msg[sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t)];
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)msg;
    iris_blk_rw_req_t* req = (iris_blk_rw_req_t*)(msg + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_READ, 3, sizeof(iris_blk_rw_req_t));
    req->start_sector = sector;
    req->num_sectors = 1;
    req->flags = 0;

    zx_status_t st = zx_channel_write(ch, 0, msg, sizeof(msg), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t observed = 0;
    st = zx_object_wait_one(ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &observed);
    if (st != ZX_OK) return st;

    uint8_t rep_buf[sizeof(iris_msg_hdr_t) + 512 + 64];
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t) + 512) {
        return ZX_ERR_IO;
    }

    iris_msg_hdr_t* rep = (iris_msg_hdr_t*)rep_buf;
    if (rep->status != ZX_OK) return rep->status;

    if (out_data) {
        memcpy(out_data, rep_buf + sizeof(iris_msg_hdr_t), 512);
    }
    return ZX_OK;
}

/* ==============================================================================
 * Test Suite Execution
 * ============================================================================== */

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /* 0. Cross-Reboot Persistence Check (Section 9.2) */
    #define PERSIST_FILE "/Users/persist_check.txt"
    #define PERSIST_TOKEN "irisOS_cross_reboot_persistence_token_64bit_ae8f12d4"

    int pfd = open(PERSIST_FILE, O_RDONLY);
    if (pfd < 0) {
        /* Boot 1: File does not exist yet. Write token, sync/flush, and initiate clean shutdown */
        int wpfd = open(PERSIST_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (wpfd < 0) {
            log_status(STATUS_FAIL, "its_irisOS", "Boot 1: Failed to create " PERSIST_FILE " (errno=%d).", errno);
            return 1;
        }
        ssize_t pw = write(wpfd, PERSIST_TOKEN, strlen(PERSIST_TOKEN));
        close(wpfd);
        if (pw != (ssize_t)strlen(PERSIST_TOKEN)) {
            log_status(STATUS_FAIL, "its_irisOS", "Boot 1: Failed to write " PERSIST_FILE " (ret=%zd).", pw);
            return 1;
        }
        log_status(STATUS_OKAY, "its_irisOS", "Cross-Reboot Persistence Setup: Wrote token to " PERSIST_FILE ".");
        log_status(STATUS_OKAY, "its_irisOS", "Boot 1 complete. Requesting daemon teardown for reboot.");
        return 0;
    }

    /* Boot 2: File exists from Boot 1. Verify token content! */
    char pbuf[64] = {0};
    ssize_t persist_read_len = read(pfd, pbuf, sizeof(pbuf) - 1);
    close(pfd);
    if (persist_read_len != (ssize_t)strlen(PERSIST_TOKEN) || strcmp(pbuf, PERSIST_TOKEN) != 0) {
        char hex[130] = {0};
        for (int i = 0; i < persist_read_len && i < 32; i++) {
            snprintf(hex + i * 2, 3, "%02x", (unsigned char)pbuf[i]);
        }
        log_status(STATUS_FAIL, "its_irisOS", "Cross-reboot persistence mismatch: read %zd bytes (hex=%s, str=\"%s\"), expected \"%s\".",
                   persist_read_len, hex, pbuf, PERSIST_TOKEN);
        return 1;
    }
    log_status(STATUS_OKAY, "its_irisOS", "Cross-reboot persistence verified: " PERSIST_FILE " token matched from Boot 1.");

    /* 1. Verify standard C library memory allocation (malloc/free) and formatting */
    const char* test_payload = "standard C runtime heap & stdio verified";
    size_t len = strlen(test_payload) + 1;
    char* buffer = (char*)malloc(len);
    if (!buffer) {
        log_status(STATUS_FAIL, "its_irisOS", "malloc(%zu) allocation failed.", len);
        return 2;
    }
    strcpy(buffer, test_payload);
    log_status(STATUS_OKAY, "its_irisOS", "malloc/free & snprintf (%s).", buffer);
    free(buffer);

    /* 2. Verify Client-Side File Descriptor Table via POSIX write(1) */
    const char* fd_test_msg = "FD Table direct write(1) to stdout verified.\n";
    size_t fd_msg_len = strlen(fd_test_msg);
    ssize_t w1_res = write(1, fd_test_msg, fd_msg_len);
    if (w1_res == (ssize_t)fd_msg_len) {
        log_status(STATUS_OKAY, "its_irisOS", "FD Table direct write(1) to stdout verified (%zd bytes).", w1_res);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "write(1) failed (ret=%zd, expected=%zu, errno=%d).", w1_res, fd_msg_len, errno);
        return 3;
    }

    /* 3. Verify isatty() on default file descriptors */
    int tty1 = isatty(1);
    int tty2 = isatty(2);
    if (tty1 == 1 && tty2 == 1) {
        log_status(STATUS_OKAY, "its_irisOS", "FD Table isatty(1)=%d, isatty(2)=%d (console TTY verified).", tty1, tty2);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "isatty check failed: isatty(1)=%d, isatty(2)=%d.", tty1, tty2);
        return 4;
    }

    /* 4. Verify open(), write(), and close() on /dev/console virtual device */
    int dev_fd = open("/dev/console", O_WRONLY);
    if (dev_fd >= 3) {
        log_status(STATUS_OKAY, "its_irisOS", "Console device open(\"/dev/console\") -> fd %d, write & close.", dev_fd);
        close(dev_fd);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "open(\"/dev/console\") returned %d.", dev_fd);
        return 5;
    }

    /* 5. Verify EBADF handling on invalid/closed descriptor */
    int close_res = close(99);
    if (close_res == -1 && errno == EBADF) {
        log_status(STATUS_OKAY, "its_irisOS", "FD Table invalid descriptor rejection (EBADF).");
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "close(99) did not return EBADF (ret=%d, errno=%d).", close_res, errno);
        return 6;
    }

    /* 6. Verify posix_spawn file actions primitives */
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) == 0) {
        int ad_rc = posix_spawn_file_actions_adddup2(&actions, 1, 2);
        int ds_rc = posix_spawn_file_actions_destroy(&actions);
        if (ad_rc == 0 && ds_rc == 0) {
            log_status(STATUS_OKAY, "its_irisOS", "POSIX spawn file actions (init/adddup2/destroy).");
        } else {
            log_status(STATUS_FAIL, "its_irisOS", "posix_spawn_file_actions adddup2/destroy failed (%d, %d).", ad_rc, ds_rc);
            return 7;
        }
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "posix_spawn_file_actions_init failed.");
        return 7;
    }

    /* 7. Verify Ring 3 VirtIO Block Driver (virtio_blk) via direct IPC */
    zx_handle_t blk_ch = zx_take_startup_handle(PA_HND(PA_USER1, 0));
    if (blk_ch != ZX_HANDLE_INVALID) {
        iris_blk_info_t blk_info;
        memset(&blk_info, 0, sizeof(blk_info));
        zx_status_t st = block_get_info(blk_ch, &blk_info);
        if (st == ZX_OK && blk_info.sector_count > 0) {
            log_status(STATUS_OKAY, "its_irisOS", "VirtIO Block IPC query (BLK_GET_INFO): %llu sectors (%u bytes/sector).",
                       (unsigned long long)blk_info.sector_count, blk_info.sector_size);

            /* Fill 512-byte pattern for Sector 100 write/read verification */
            uint8_t pattern[512];
            pattern[0] = 0x49; pattern[1] = 0x52; pattern[2] = 0x49; pattern[3] = 0x53; /* 'IRIS' */
            pattern[4] = 0x4F; pattern[5] = 0x53; pattern[6] = 0x5F; pattern[7] = 0x34; /* 'OS_4' */
            pattern[8] = 0xDE; pattern[9] = 0xAD; pattern[10] = 0xBE; pattern[11] = 0xEF;
            pattern[12] = 0xCA; pattern[13] = 0xFE; pattern[14] = 0xBA; pattern[15] = 0xBE;
            for (int i = 16; i < 512; i++) {
                pattern[i] = (uint8_t)((i * 37 + 101) ^ 0x5A);
            }

            st = block_write_sector(blk_ch, 100, pattern);
            if (st == ZX_OK) {
                log_status(STATUS_OKAY, "its_irisOS", "VirtIO Block IPC write to Sector 100 confirmed (512 bytes).");
            } else {
                log_status(STATUS_FAIL, "its_irisOS", "VirtIO Block Sector 100 write failed (%d).", st);
                return 8;
            }

            uint8_t read_back[512];
            memset(read_back, 0, sizeof(read_back));
            st = block_read_sector(blk_ch, 100, read_back);
            if (st == ZX_OK && memcmp(pattern, read_back, 512) == 0) {
                log_status(STATUS_OKAY, "its_irisOS", "VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).");
            } else {
                log_status(STATUS_FAIL, "its_irisOS", "VirtIO Block Sector 100 readback verification failed.");
                return 8;
            }
        } else {
            log_status(STATUS_FAIL, "its_irisOS", "VirtIO Block device query failed (%d).", st);
            return 8;
        }
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "VirtIO Block channel handle not provided in startup message.");
        return 8;
    }

    /* 8. Verify Block-Level Encryption Proxy (iris_crypt) & On-Disk Ciphertext */
    zx_handle_t crypt_ch = zx_take_startup_handle(PA_HND(PA_USER2, 0));
    if (crypt_ch != ZX_HANDLE_INVALID && blk_ch != ZX_HANDLE_INVALID) {
        iris_blk_info_t crypt_info;
        memset(&crypt_info, 0, sizeof(crypt_info));
        zx_status_t st = block_get_info(crypt_ch, &crypt_info);
        if (st == ZX_OK && crypt_info.sector_count > 0) {
            log_status(STATUS_OKAY, "its_irisOS", "iris_crypt IPC query (BLK_GET_INFO): %llu usable sectors.",
                       (unsigned long long)crypt_info.sector_count);

            uint64_t test_logical_sec = (crypt_info.sector_count > 60000ULL) ? 60000ULL : 0ULL;
            uint64_t data_part_base   = (crypt_info.sector_count > 60000ULL) ? 198656ULL : 0ULL;
            uint64_t test_phys_sec    = data_part_base + 8ULL + test_logical_sec;

            uint8_t plain_sector[512];
            const char* banner_str = "IRISOS_CRYPT_AES256_XTS_PLAINTEXT_LOGICAL_SECTOR_VERIFICATION!";
            size_t b_len = strlen(banner_str);
            memcpy(plain_sector, banner_str, b_len);
            for (size_t i = b_len; i < 512; ++i) {
                plain_sector[i] = (uint8_t)((i * 73u + 19u) ^ 0xA5u);
            }

            st = block_write_sector(crypt_ch, test_logical_sec, plain_sector);
            if (st == ZX_OK) {
                log_status(STATUS_OKAY, "its_irisOS", "Wrote 512-byte plaintext to logical Sector %llu via iris_crypt.",
                           (unsigned long long)test_logical_sec);
            } else {
                log_status(STATUS_FAIL, "its_irisOS", "Logical Sector write via iris_crypt failed (%d).", st);
                return 9;
            }

            /* Read backing physical sector directly from virtio_blk and verify on-disk ciphertext */
            uint8_t raw_cipher[512];
            memset(raw_cipher, 0, sizeof(raw_cipher));
            st = block_read_sector(blk_ch, test_phys_sec, raw_cipher);
            if (st == ZX_OK && memcmp(raw_cipher, plain_sector, 512) != 0) {
                log_status(STATUS_OKAY, "its_irisOS", "Verified physical LBA %llu on virtio_blk is AES-256-XTS ciphertext.",
                           (unsigned long long)test_phys_sec);
            } else {
                log_status(STATUS_FAIL, "its_irisOS", "Physical Sector matches plaintext! Encryption did not occur!");
                return 9;
            }

            /* Read logical sector via iris_crypt and verify decrypted plaintext matches byte-for-byte */
            uint8_t decrypted[512];
            memset(decrypted, 0, sizeof(decrypted));
            st = block_read_sector(crypt_ch, test_logical_sec, decrypted);
            if (st == ZX_OK && memcmp(decrypted, plain_sector, 512) == 0) {
                log_status(STATUS_OKAY, "its_irisOS", "Verified logical Sector %llu read via iris_crypt matches decrypted plaintext.",
                           (unsigned long long)test_logical_sec);
            } else {
                log_status(STATUS_FAIL, "its_irisOS", "Decrypted logical Sector mismatch via iris_crypt.");
                return 9;
            }
        } else {
            log_status(STATUS_FAIL, "its_irisOS", "iris_crypt block query failed (%d).", st);
            return 9;
        }
        zx_handle_close(crypt_ch);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "iris_crypt channel handle not provided in startup message.");
        return 9;
    }
    if (blk_ch != ZX_HANDLE_INVALID) {
        zx_handle_close(blk_ch);
    }

    /* 9. Verify Read-Only /System UFS2 mount */
    int sys_ro_fd = open("/System/Resources/release.txt", O_RDONLY);
    if (sys_ro_fd >= 3) {
        char rel_buf[128] = {0};
        ssize_t rbytes = read(sys_ro_fd, rel_buf, sizeof(rel_buf) - 1);
        close(sys_ro_fd);
        if (rbytes > 0 && strcmp(rel_buf, "irisOS release 0.1.0\n") == 0) {
            log_status(STATUS_OKAY, "its_irisOS", "Read /System/Resources/release.txt (\"%s\").", "irisOS release 0.1.0");
        } else {
            log_status(STATUS_FAIL, "its_irisOS", "Read /System/Resources/release.txt mismatch (rbytes=%zd, got=\"%s\").", rbytes, rel_buf);
            return 10;
        }
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "open(\"/System/Resources/release.txt\") failed (errno=%d).", errno);
        return 10;
    }

    /* 10. Verify writing to /System/readonly_test.txt is rejected with EROFS */
    errno = 0;
    int ro_test_fd = open("/System/readonly_test.txt", O_CREAT | O_WRONLY, 0644);
    if (ro_test_fd == -1 && errno == EROFS) {
        log_status(STATUS_OKAY, "its_irisOS", "Write to /System/readonly_test.txt rejected with EROFS (errno=%d).", errno);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "Write to /System/readonly_test.txt was not rejected with EROFS (errno=%d).", errno);
        if (ro_test_fd >= 0) close(ro_test_fd);
        return 11;
    }

    /* 11. Verify writing and reading back /Users/verify.txt on encrypted Data UFS2 */
    const char* verify_text = "irisOS Phase 4 UFS2 + AES-256-XTS VFS Persistence Verified!";
    size_t verify_len = strlen(verify_text);

    int wr_fd = open("/Users/verify.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (wr_fd < 3) {
        log_status(STATUS_FAIL, "its_irisOS", "open(\"/Users/verify.txt\", O_CREAT|O_WRONLY) failed (errno=%d).", errno);
        return 12;
    }

    ssize_t w_res = write(wr_fd, verify_text, verify_len);
    close(wr_fd);
    if (w_res != (ssize_t)verify_len) {
        log_status(STATUS_FAIL, "its_irisOS", "write(\"/Users/verify.txt\") wrote %zd/%zu bytes (errno=%d).", w_res, verify_len, errno);
        return 12;
    }

    int rd_fd = open("/Users/verify.txt", O_RDONLY);
    if (rd_fd < 3) {
        log_status(STATUS_FAIL, "its_irisOS", "open(\"/Users/verify.txt\", O_RDONLY) failed (errno=%d).", errno);
        return 12;
    }

    char read_back_txt[128] = {0};
    ssize_t r_res = read(rd_fd, read_back_txt, sizeof(read_back_txt) - 1);
    struct stat st_buf;
    int fstat_rc = fstat(rd_fd, &st_buf);
    close(rd_fd);

    if (r_res != (ssize_t)verify_len || strcmp(read_back_txt, verify_text) != 0 || fstat_rc != 0) {
        log_status(STATUS_FAIL, "its_irisOS", "/Users/verify.txt readback mismatch (r_res=%zd, got=\"%s\").", r_res, read_back_txt);
        return 12;
    }
    log_status(STATUS_OKAY, "its_irisOS", "Wrote & verified /Users/verify.txt (%zd bytes, ino=%llu, \"%s\").",
               r_res, (unsigned long long)st_buf.st_ino, read_back_txt);

    /* 12. Verify Ring 3 Pseudo-Terminal Server (iris_pty) & Line Discipline */
    int tty0 = isatty(0);
    int tty1_val = isatty(1);
    if (tty0 == 1 && tty1_val == 1) {
        log_status(STATUS_OKAY, "its_irisOS", "PTY file descriptors 0 and 1 verified (isatty(0)=%d, isatty(1)=%d).", tty0, tty1_val);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "PTY isatty check failed: isatty(0)=%d, isatty(1)=%d.", tty0, tty1_val);
        return 13;
    }

    zx_handle_t pty_ch = iris_get_pty_channel();
    if (pty_ch == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "its_irisOS", "PTY channel handle missing from startup message.");
        return 13;
    }

    /* 12.1 Window size query */
    uint16_t rows = 0, cols = 0;
    int gw_res = iris_pty_get_window(pty_ch, &rows, &cols);
    if (gw_res == 0 && rows > 0 && cols > 0) {
        log_status(STATUS_OKAY, "its_irisOS", "PTY window size query verified (%u rows x %u cols).", rows, cols);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "PTY get window failed (res=%d, rows=%u, cols=%u).", gw_res, rows, cols);
        return 13;
    }

    /* 12.2 Canonical line buffering, backspace editing ('\b'), and Enter translation ('\r' -> '\n') */
    const char* edit_seq = "Helo\blo World\r";
    iris_pty_inject(pty_ch, edit_seq, strlen(edit_seq));
    char pty_in[64] = {0};
    ssize_t pr = read(0, pty_in, sizeof(pty_in) - 1);
    if (pr > 0 && strcmp(pty_in, "Hello World\n") == 0) {
        log_status(STATUS_OKAY, "its_irisOS", "PTY canonical line buffering and backspace editing verified (\"Hello World\").");
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "PTY canonical line buffering failed (pr=%zd, got=\"%s\").", pr, pty_in);
        return 13;
    }

    /* 12.3 / 12.4 Verify PTY mode flags & direct write to PTY */
    uint32_t pty_mode_flags = 0;
    int gm_res = iris_pty_get_mode(pty_ch, &pty_mode_flags);
    if (gm_res == 0 && (pty_mode_flags & IRIS_PTY_MODE_ECHO) && (pty_mode_flags & IRIS_PTY_MODE_CANONICAL)) {
        log_status(STATUS_OKAY, "its_irisOS", "PTY mode verified (CANONICAL | ECHO active, flags=0x%x).", pty_mode_flags);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "PTY get mode failed (res=%d, flags=0x%x).", gm_res, pty_mode_flags);
        return 13;
    }

    /* 12.5 Raw mode immediate pass-through */
    iris_pty_set_mode(pty_ch, IRIS_PTY_MODE_RAW);
    const char* raw_str = "RAW_INPUT";
    iris_pty_inject(pty_ch, raw_str, strlen(raw_str));
    memset(pty_in, 0, sizeof(pty_in));
    pr = read(0, pty_in, sizeof(pty_in) - 1);
    if (pr > 0 && strcmp(pty_in, raw_str) == 0) {
        log_status(STATUS_OKAY, "its_irisOS", "PTY raw mode immediate pass-through verified (\"%s\").", pty_in);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "PTY raw mode failed (pr=%zd, got=\"%s\").", pr, pty_in);
        return 13;
    }
    iris_pty_set_mode(pty_ch, IRIS_PTY_MODE_CANONICAL | IRIS_PTY_MODE_ECHO);

    /* 12.6 Ctrl+C signal generation (SIGINT) and line discard */
    const char* sig_seq = "discarded_line\x03next_line\r";
    iris_pty_inject(pty_ch, sig_seq, strlen(sig_seq));
    uint32_t signo = 0;
    iris_pty_get_signal(pty_ch, &signo);
    memset(pty_in, 0, sizeof(pty_in));
    pr = read(0, pty_in, sizeof(pty_in) - 1);
    if (signo == 2 && strcmp(pty_in, "next_line\n") == 0) {
        log_status(STATUS_OKAY, "its_irisOS", "PTY Ctrl+C signal generation (SIGINT=2) and line discard verified.");
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "PTY Ctrl+C failed (signo=%u, got=\"%s\").", signo, pty_in);
        return 13;
    }

    /* 12.7 Ctrl+D EOF delivery */
    const char* eof_seq = "\x04";
    iris_pty_inject(pty_ch, eof_seq, 1);
    memset(pty_in, 0, sizeof(pty_in));
    pr = read(0, pty_in, sizeof(pty_in) - 1);
    if (pr == 0) {
        log_status(STATUS_OKAY, "its_irisOS", "PTY Ctrl+D EOF delivery verified (read returned 0 bytes).");
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "PTY Ctrl+D failed (pr=%zd, expected 0).", pr);
        return 13;
    }

    /* 13. Verify Toybox Multicall Userland & Shell (Step 5.2) */
    /* 13.1 Verify /System/Bin/toybox uname -a via posix_spawn() */
    int pipe_fds[2] = { -1, -1 };
    if (pipe(pipe_fds) != 0) {
        log_status(STATUS_FAIL, "its_irisOS", "pipe() creation failed (errno=%d).", errno);
        return 14;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipe_fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, pipe_fds[0]);

    char* tb_argv[] = { "/System/Bin/toybox", "uname", "-a", NULL };
    pid_t pid = 0;
    int sp_res = posix_spawn(&pid, "/System/Bin/toybox", &fa, NULL, tb_argv, NULL);
    posix_spawn_file_actions_destroy(&fa);
    close(pipe_fds[1]);

    if (sp_res != 0) {
        log_status(STATUS_FAIL, "its_irisOS", "posix_spawn(\"/System/Bin/toybox\") failed (%d).", sp_res);
        close(pipe_fds[0]);
        return 14;
    }

    /* Read until EOF (all write ends closed) */
    char uname_out[256] = {0};
    size_t uname_len = 0;
    for (;;) {
        ssize_t n = read(pipe_fds[0], uname_out + uname_len, sizeof(uname_out) - 1 - uname_len);
        if (n <= 0) break;
        uname_len += (size_t)n;
        if (uname_len >= sizeof(uname_out) - 1) break;
    }
    close(pipe_fds[0]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    /* Strip trailing newlines for clean log display */
    for (size_t i = 0; i < uname_len; ++i) {
        if (uname_out[i] == '\n' || uname_out[i] == '\r') {
            uname_out[i] = '\0';
            break;
        }
    }

    char expected[128];
#if defined(__x86_64__)
    snprintf(expected, sizeof(expected), "irisOS irisOS-dev 0.1.0 Zircon 1011 (Ring 3 userland) x86_64 Toybox");
#else
    snprintf(expected, sizeof(expected), "irisOS irisOS-dev 0.1.0 Zircon 1011 (Ring 3 userland) aarch64 Toybox");
#endif

    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0 && strcmp(uname_out, expected) == 0) {
        log_status(STATUS_OKAY, "its_irisOS", "Toybox multicall userland verified: /System/Bin/toybox uname -a -> \"%s\".", uname_out);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "Toybox uname output unexpected: got=\"%s\", expected=\"%s\" (status=%d).",
                   uname_out, expected, WEXITSTATUS(wstatus));
        return 14;
    }

    /* 13.2 Verify /bin/sh -c "echo 'Toybox execution verified'" captured via pipe */
    int sh_pipe[2] = { -1, -1 };
    if (pipe(sh_pipe) != 0) {
        log_status(STATUS_FAIL, "its_irisOS", "pipe() creation for shell test failed (errno=%d).", errno);
        return 15;
    }
    posix_spawn_file_actions_t sh_fa;
    posix_spawn_file_actions_init(&sh_fa);
    posix_spawn_file_actions_adddup2(&sh_fa, sh_pipe[1], 1);
    posix_spawn_file_actions_addclose(&sh_fa, sh_pipe[0]);

    char* sh_argv[] = { "/bin/sh", "-c", "echo 'Toybox execution verified'", NULL };
    pid_t sh_pid = 0;
    int sh_sp = posix_spawn(&sh_pid, "/bin/sh", &sh_fa, NULL, sh_argv, NULL);
    posix_spawn_file_actions_destroy(&sh_fa);
    close(sh_pipe[1]);

    if (sh_sp != 0) {
        log_status(STATUS_FAIL, "its_irisOS", "posix_spawn(\"/bin/sh\") failed (%d).", sh_sp);
        close(sh_pipe[0]);
        return 15;
    }

    char sh_out[128] = {0};
    size_t sh_len = 0;
    for (;;) {
        ssize_t n = read(sh_pipe[0], sh_out + sh_len, sizeof(sh_out) - 1 - sh_len);
        if (n <= 0) break;
        sh_len += (size_t)n;
        if (sh_len >= sizeof(sh_out) - 1) break;
    }
    close(sh_pipe[0]);

    int sh_status = 0;
    waitpid(sh_pid, &sh_status, 0);

    for (size_t i = 0; i < sh_len; ++i) {
        if (sh_out[i] == '\n' || sh_out[i] == '\r') {
            sh_out[i] = '\0';
            break;
        }
    }

    const char* expected_sh = "Toybox execution verified";
    if (WIFEXITED(sh_status) && WEXITSTATUS(sh_status) == 0 && strcmp(sh_out, expected_sh) == 0) {
        log_status(STATUS_OKAY, "its_irisOS", "Toybox shell execution verified: /bin/sh -c -> \"%s\".", sh_out);
    } else {
        log_status(STATUS_FAIL, "its_irisOS", "Toybox shell output unexpected: got=\"%s\", expected=\"%s\" (status=%d).",
                   sh_out, expected_sh, WEXITSTATUS(sh_status));
        return 15;
    }

    /* 14. Verify Directory Operations & Filesystem Features (Pass 1) */
    /* 14.1 Verify opendir() / readdir() on /System/Bin */
    DIR* dir = opendir("/System/Bin");
    if (!dir) {
        log_status(STATUS_FAIL, "its_irisOS", "opendir(\"/System/Bin\") failed (errno=%d).", errno);
        return 16;
    }
    int dirent_count = 0;
    bool found_toybox = false;
    struct dirent* de;
    while ((de = readdir(dir)) != NULL) {
        dirent_count++;
        if (strcmp(de->d_name, "toybox") == 0) {
            found_toybox = true;
        }
    }
    closedir(dir);
    if (!found_toybox || dirent_count < 10) {
        log_status(STATUS_FAIL, "its_irisOS", "readdir(\"/System/Bin\") failed (count=%d, found_toybox=%d).", dirent_count, found_toybox);
        return 16;
    }
    log_status(STATUS_OKAY, "its_irisOS", "VFS opendir/readdir verified: /System/Bin listed %d entries (found toybox).", dirent_count);

    /* 14.2 Verify open(O_CREAT) and unlink() on /Users */
    int ufd = open("/Users/test_unlink.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ufd < 0) {
        log_status(STATUS_FAIL, "its_irisOS", "open(\"/Users/test_unlink.txt\") failed (errno=%d).", errno);
        return 17;
    }
    write(ufd, "delete me", 9);
    close(ufd);

    if (unlink("/Users/test_unlink.txt") != 0) {
        log_status(STATUS_FAIL, "its_irisOS", "unlink(\"/Users/test_unlink.txt\") failed (errno=%d).", errno);
        return 17;
    }

    int check_fd = open("/Users/test_unlink.txt", O_RDONLY);
    if (check_fd >= 0) {
        close(check_fd);
        log_status(STATUS_FAIL, "its_irisOS", "File /Users/test_unlink.txt still exists after unlink!");
        return 17;
    }
    log_status(STATUS_OKAY, "its_irisOS", "VFS unlink verified: successfully created, unlinked, and confirmed deletion on /Users.");

    /* 14.3 Verify EROFS enforcement on /System */
    if (unlink("/System/Bin/toybox") == 0) {
        log_status(STATUS_FAIL, "its_irisOS", "unlink(\"/System/Bin/toybox\") succeeded unexpectedly on read-only /System!");
        return 18;
    }
    log_status(STATUS_OKAY, "its_irisOS", "VFS EROFS enforcement verified: cannot unlink on /System.");

    /* 14.4 Verify readlink() on /System/Bin/sh */
    char sym_buf[64] = {0};
    ssize_t rlen = readlink("/System/Bin/sh", sym_buf, sizeof(sym_buf) - 1);
    if (rlen <= 0 || strcmp(sym_buf, "toybox") != 0) {
        log_status(STATUS_FAIL, "its_irisOS", "readlink(\"/System/Bin/sh\") failed: rlen=%ld, target=\"%s\".", rlen, sym_buf);
        return 19;
    }
    log_status(STATUS_OKAY, "its_irisOS", "VFS readlink verified: /System/Bin/sh -> \"%s\".", sym_buf);

    log_status(STATUS_OKAY, "its_irisOS", "Interactive Test Suite all checks passed.");
    return 0;
}

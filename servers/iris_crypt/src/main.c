/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_crypt/src/main.c
 * Purpose: Standalone Ring 3 Block-Level AES-256-XTS Encryption Proxy Daemon.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
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

/**
 * @file main.c
 * @brief Standalone Ring 3 AES-256-XTS Block Encryption Proxy (`bin/iris_crypt`).
 *
 * MODULE LIFECYCLE, CHANNEL ENDPOINTS & CAPABILITY DEPENDENCIES:
 *  1. Process Isolation & Capabilities:
 *     - Runs as an unprivileged Ring 3 (x86_64) / EL0 (arm64) user-space server
 *       without direct hardware MMIO or Root Resource privileges.
 *     - `PA_HND(PA_USER0, 0)`: Upstream IPC server channel endpoint (`g_server_channel`)
 *       receiving block I/O requests from `iris_fshost` or `iris_init`.
 *     - `PA_HND(PA_USER1, 0)`: Downstream IPC client channel endpoint (`g_blk_channel`)
 *       connected to the hardware block driver (`bin/virtio_blk`).
 *
 *  2. Sector 0 Metadata Header (4096 bytes / Physical Sectors 0..7):
 *     - Reserves the first 4 KB (8 x 512-byte sectors) of the backing block device
 *       for `iris_crypt_header_t` (magic `0x5450595243534952`, "IRISCRYPT").
 *     - Stores the 32-byte Argon2 salt, iteration/memory/lane cost parameters,
 *       64-byte AES-256-XTS wrapped master key, and SHA-256 integrity tags.
 *     - Implements a deterministic development test key fallback so automated QEMU
 *       boots format/unlock Sector 0 and mount without waiting for interactive input.
 *
 *  3. Transparent Block Proxy & LBA Offset Translation (`IRIS_PROTO_DRIVER`):
 *     - Exposes `logical_sectors = physical_sectors - 8` via `IRIS_BLK_OP_GET_INFO`.
 *     - Translates logical sector $L$ to physical sector $P = L + 8$ on `virtio_blk`.
 *     - Encrypts every 512-byte sector on `IRIS_BLK_OP_WRITE` using Mbed-TLS
 *       AES-256-XTS (`mbedtls_aes_crypt_xts`) with the 64-bit little-endian logical
 *       sector LBA encoded into the 16-byte XTS `data_unit` tweak.
 *     - Decrypts every 512-byte sector on `IRIS_BLK_OP_READ` using AES-256-XTS
 *       before returning plaintext to the upstream caller.
 *     - Forwards `IRIS_BLK_OP_FLUSH` transparently to `virtio_blk`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iris/ipc.h>
#include <iris/init.h>
#include <iris/log.h>

#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "servers/iris_crypt/include/iris_crypt.h"

/* Channel endpoints */
#define MAX_CRYPT_CLIENTS 4
static zx_handle_t g_client_channels[MAX_CRYPT_CLIENTS];
static uint32_t    g_num_clients    = 0;
static zx_handle_t g_blk_channel    = ZX_HANDLE_INVALID;

/* Downstream block device & GPT Data partition geometry */
static uint64_t g_part_start_sector    = 0;
static uint64_t g_phys_sector_count    = 0;
static uint64_t g_logical_sector_count = 0;
static uint32_t g_sector_size          = 512u;
static uint32_t g_max_transfer         = 120u;
static uint32_t g_next_txid            = 1000u;

/* Cryptographic state */
static bool                    g_unlocked = false;
static uint8_t                 g_master_key[IRIS_CRYPT_MASTER_KEY_LEN];
static mbedtls_aes_xts_context g_xts_enc;
static mbedtls_aes_xts_context g_xts_dec;

/* ==============================================================================
 * Downstream VirtIO Block IPC Helpers
 * ============================================================================== */

/**
 * @brief Query downstream virtio_blk device geometry via IRIS_BLK_OP_GET_INFO.
 */
static zx_status_t downstream_get_info(iris_blk_info_t* out_info) {
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_DRIVER, IRIS_BLK_OP_GET_INFO, g_next_txid++, 0);

    zx_status_t st = zx_channel_write(g_blk_channel, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) {
        return st;
    }

    zx_signals_t obs = 0;
    st = zx_object_wait_one(g_blk_channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) {
        return ZX_ERR_PEER_CLOSED;
    }

    uint8_t rep_buf[128] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(g_blk_channel, 0, rep_buf, NULL, sizeof(rep_buf), 0,
                         &act_bytes, &act_handles);
    if (st != ZX_OK) {
        return st;
    }

    if (act_bytes < sizeof(iris_msg_hdr_t) + sizeof(iris_blk_info_t)) {
        return ZX_ERR_IO;
    }

    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    if (rep_hdr->status != ZX_OK) {
        return rep_hdr->status;
    }

    memcpy(out_info, rep_buf + sizeof(iris_msg_hdr_t), sizeof(iris_blk_info_t));
    return ZX_OK;
}

/**
 * @brief Read physical sectors from downstream virtio_blk.
 */
static zx_status_t downstream_read_sectors(uint64_t phys_sector, uint32_t num_sectors, void* out_buf) {
    uint8_t req_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t)] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
    iris_blk_rw_req_t* rw = (iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_READ, g_next_txid++, sizeof(iris_blk_rw_req_t));
    rw->start_sector = phys_sector;
    rw->num_sectors  = num_sectors;
    rw->flags        = 0;

    zx_status_t st = zx_channel_write(g_blk_channel, 0, req_buf, sizeof(req_buf), NULL, 0);
    if (st != ZX_OK) {
        return st;
    }

    zx_signals_t obs = 0;
    st = zx_object_wait_one(g_blk_channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) {
        return ZX_ERR_PEER_CLOSED;
    }

    static uint8_t rep_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(g_blk_channel, 0, rep_buf, NULL, sizeof(rep_buf), 0,
                         &act_bytes, &act_handles);
    if (st != ZX_OK) {
        return st;
    }

    uint32_t expected_bytes = num_sectors * IRIS_CRYPT_SECTOR_SIZE;
    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        return ZX_ERR_IO;
    }

    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    if (rep_hdr->status != ZX_OK) {
        return rep_hdr->status;
    }
    if (act_bytes < sizeof(iris_msg_hdr_t) + expected_bytes) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out_buf, rep_buf + sizeof(iris_msg_hdr_t), expected_bytes);
    return ZX_OK;
}

/**
 * @brief Write physical sectors to downstream virtio_blk.
 */
static zx_status_t downstream_write_sectors(uint64_t phys_sector, uint32_t num_sectors, const void* in_buf) {
    uint32_t data_bytes = num_sectors * IRIS_CRYPT_SECTOR_SIZE;
    uint32_t total_msg_bytes = (uint32_t)(sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t) + data_bytes);
    if (total_msg_bytes > IRIS_IPC_MAX_MSG_BYTES) {
        return ZX_ERR_INVALID_ARGS;
    }

    static uint8_t req_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
    iris_blk_rw_req_t* rw = (iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_WRITE,
                      g_next_txid++, (uint32_t)sizeof(iris_blk_rw_req_t) + data_bytes);
    rw->start_sector = phys_sector;
    rw->num_sectors  = num_sectors;
    rw->flags        = 0;
    memcpy(rw + 1, in_buf, data_bytes);

    zx_status_t st = zx_channel_write(g_blk_channel, 0, req_buf, total_msg_bytes, NULL, 0);
    if (st != ZX_OK) {
        return st;
    }

    zx_signals_t obs = 0;
    st = zx_object_wait_one(g_blk_channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) {
        return ZX_ERR_PEER_CLOSED;
    }

    uint8_t rep_buf[128] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(g_blk_channel, 0, rep_buf, NULL, sizeof(rep_buf), 0,
                         &act_bytes, &act_handles);
    if (st != ZX_OK) {
        return st;
    }
    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        return ZX_ERR_IO;
    }

    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    return rep_hdr->status;
}

/**
 * @brief Flush downstream virtio_blk cache.
 */
static zx_status_t downstream_flush(void) {
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_DRIVER, IRIS_BLK_OP_FLUSH, g_next_txid++, 0);

    zx_status_t st = zx_channel_write(g_blk_channel, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) {
        return st;
    }

    zx_signals_t obs = 0;
    st = zx_object_wait_one(g_blk_channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) {
        return ZX_ERR_PEER_CLOSED;
    }

    uint8_t rep_buf[128] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(g_blk_channel, 0, rep_buf, NULL, sizeof(rep_buf), 0,
                         &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t)) {
        return ZX_ERR_IO;
    }

    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    return rep_hdr->status;
}

/* ==============================================================================
 * Cryptographic Key Derivation, Header Initialization & AES-256-XTS Operations
 * ============================================================================== */

/**
 * @brief Construct the 16-byte IEEE P1619 / NIST SP 800-38E XTS tweak (`data_unit`)
 *        from a 64-bit sector LBA in little-endian byte order.
 */
static inline void make_xts_data_unit(uint64_t sector_lba, uint8_t data_unit[16]) {
    memset(data_unit, 0, 16);
    for (unsigned i = 0; i < 8; ++i) {
        data_unit[i] = (uint8_t)((sector_lba >> (i * 8u)) & 0xFFu);
    }
}

/**
 * @brief Compute SHA-256 integrity checksum over the header metadata fields
 *        (bytes 0..offsetof(iris_crypt_header_t, header_checksum)).
 */
static void compute_header_checksum(const iris_crypt_header_t* hdr, uint8_t out_hash[32]) {
    size_t len = offsetof(iris_crypt_header_t, header_checksum);
    mbedtls_sha256((const uint8_t*)hdr, len, out_hash, 0 /* SHA-256 */);
}

/**
 * @brief Inspect LBA 1..5 for a GPT partition table and locate Partition 4 ("Data").
 *        If no GPT is found, defaults to the whole block device starting at LBA 0.
 */
static void detect_gpt_data_partition(void) {
    uint8_t gpt_buf[512 * 4] __attribute__((aligned(8)));
    if (downstream_read_sectors(1, 4, gpt_buf) != ZX_OK) {
        return;
    }

    /* Check "EFI PART" signature at LBA 1 (offset 0 of gpt_buf) */
    if (memcmp(gpt_buf, "EFI PART", 8) != 0) {
        return;
    }

    /* Partition entries start at LBA 2 (offset 512 of gpt_buf), 128 bytes each */
    const uint8_t* entries = gpt_buf + 512;
    for (uint32_t i = 0; i < 12; ++i) {
        const uint8_t* ent = entries + (i * 128);
        uint64_t first_lba = 0;
        uint64_t last_lba  = 0;
        memcpy(&first_lba, ent + 32, sizeof(uint64_t));
        memcpy(&last_lba,  ent + 40, sizeof(uint64_t));
        if (first_lba == 0 || last_lba <= first_lba) {
            continue;
        }
        /* Match UTF-16LE name "Data" at offset 56 */
        const uint16_t* name16 = (const uint16_t*)(ent + 56);
        if (name16[0] == 'D' && name16[1] == 'a' && name16[2] == 't' &&
            name16[3] == 'a' && name16[4] == 0) {
            g_part_start_sector    = first_lba;
            g_phys_sector_count    = (last_lba - first_lba) + 1ULL;
            g_logical_sector_count = g_phys_sector_count - IRIS_CRYPT_HEADER_SECTORS;
            return;
        }
    }
}

/**
 * @brief Initialize or unlock the 4 KB Sector 0 metadata header using Argon2id
 *        and the Phase 4 deterministic development key fallback.
 */
static zx_status_t init_or_unlock_metadata_header(void) {
    static iris_crypt_header_t hdr __attribute__((aligned(8)));
    memset(&hdr, 0, sizeof(hdr));

    /* 1. Read physical Sectors 0..7 (4096 bytes) of the target partition from virtio_blk */
    zx_status_t st = downstream_read_sectors(g_part_start_sector, IRIS_CRYPT_HEADER_SECTORS, &hdr);
    if (st != ZX_OK) {
        log_status(STATUS_FAIL, "iris_crypt", "Failed to read Sector 0 metadata header (%d).", st);
        return st;
    }

    const uint8_t* dev_pw = (const uint8_t*)IRIS_CRYPT_DEV_PASSPHRASE;
    size_t dev_pw_len = strlen(IRIS_CRYPT_DEV_PASSPHRASE);

    bool need_format = false;
    if (hdr.magic != IRIS_CRYPT_HEADER_MAGIC || hdr.version != IRIS_CRYPT_HEADER_VERSION) {
        need_format = true;
    } else {
        uint8_t expected_csum[IRIS_CRYPT_SHA256_LEN];
        compute_header_checksum(&hdr, expected_csum);
        if (memcmp(expected_csum, hdr.header_checksum, IRIS_CRYPT_SHA256_LEN) != 0) {
            need_format = true;
        }
    }

    if (need_format) {
        /* Format new 4 KB Sector 0 metadata header with deterministic test vector */
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic           = IRIS_CRYPT_HEADER_MAGIC;
        hdr.version         = IRIS_CRYPT_HEADER_VERSION;
        hdr.flags           = IRIS_CRYPT_FLAG_DEV_KEY;
        hdr.argon2_t_cost   = IRIS_CRYPT_DEFAULT_T_COST;
        hdr.argon2_m_cost   = IRIS_CRYPT_DEFAULT_M_COST;
        hdr.argon2_lanes    = IRIS_CRYPT_DEFAULT_LANES;
        hdr.sector_offset   = IRIS_CRYPT_HEADER_SECTORS;
        hdr.logical_sectors = g_logical_sector_count;

        /* Deterministic 32-byte salt for reproducible automated Phase 4 test vector */
        static const uint8_t k_dev_salt[IRIS_CRYPT_SALT_LEN] = {
            0x49, 0x52, 0x49, 0x53, 0x4F, 0x53, 0x5F, 0x41,
            0x52, 0x47, 0x4F, 0x4E, 0x32, 0x5F, 0x53, 0x41,
            0x4C, 0x54, 0x5F, 0x56, 0x31, 0x5F, 0x32, 0x30,
            0x32, 0x36, 0x5F, 0x42, 0x4C, 0x4F, 0x43, 0x4B
        };
        memcpy(hdr.argon2_salt, k_dev_salt, IRIS_CRYPT_SALT_LEN);

        /* Derive 512-bit (64-byte) Key Encryption Key (KEK) via Argon2id */
        uint8_t kek[IRIS_CRYPT_MASTER_KEY_LEN];
        if (iris_argon2id_derive(dev_pw, dev_pw_len,
                                 hdr.argon2_salt, IRIS_CRYPT_SALT_LEN,
                                 hdr.argon2_t_cost, hdr.argon2_m_cost, hdr.argon2_lanes,
                                 kek, sizeof(kek)) != 0) {
            log_status(STATUS_FAIL, "iris_crypt", "Argon2id KEK derivation failed.");
            return ZX_ERR_INTERNAL;
        }

        /* Generate deterministic 512-bit (64-byte) AES-256-XTS Master Key (Key1 || Key2) */
        mbedtls_sha256((const uint8_t*)"irisOS-AES256-XTS-MasterKey-Part1-Data", 38,
                       g_master_key, 0);
        mbedtls_sha256((const uint8_t*)"irisOS-AES256-XTS-MasterKey-Part2-Tweak", 39,
                       g_master_key + 32, 0);

        /* Compute SHA-256 verification digest of unwrapped master key */
        mbedtls_sha256(g_master_key, IRIS_CRYPT_MASTER_KEY_LEN, hdr.key_verification_tag, 0);

        /* Wrap 64-byte Master Key using KEK with AES-256-XTS (data_unit = 0) */
        mbedtls_aes_xts_context kek_xts;
        mbedtls_aes_xts_init(&kek_xts);
        if (mbedtls_aes_xts_setkey_enc(&kek_xts, kek, 512) != 0) {
            mbedtls_aes_xts_free(&kek_xts);
            mbedtls_platform_zeroize(kek, sizeof(kek));
            return ZX_ERR_INTERNAL;
        }
        uint8_t zero_tweak[16] = {0};
        mbedtls_aes_crypt_xts(&kek_xts, MBEDTLS_AES_ENCRYPT, IRIS_CRYPT_MASTER_KEY_LEN,
                              zero_tweak, g_master_key, hdr.encrypted_master_key);
        mbedtls_aes_xts_free(&kek_xts);
        mbedtls_platform_zeroize(kek, sizeof(kek));

        /* Compute header SHA-256 integrity checksum */
        compute_header_checksum(&hdr, hdr.header_checksum);

        /* Write 4 KB header to physical Sectors 0..7 on virtio_blk */
        st = downstream_write_sectors(g_part_start_sector, IRIS_CRYPT_HEADER_SECTORS, &hdr);
        if (st != ZX_OK) {
            log_status(STATUS_FAIL, "iris_crypt", "Failed to write Sector 0 metadata header (%d).", st);
            return st;
        }

        log_status(STATUS_OKAY, "iris_crypt", "Initialized 4 KB Sector 0 header (magic 0x%016llx, Argon2id + AES-256-XTS).",
                   (unsigned long long)hdr.magic);
    } else {
        /* Header already present on disk: derive KEK via Argon2id and unwrap Master Key */
        uint8_t kek[IRIS_CRYPT_MASTER_KEY_LEN];
        if (iris_argon2id_derive(dev_pw, dev_pw_len,
                                 hdr.argon2_salt, IRIS_CRYPT_SALT_LEN,
                                 hdr.argon2_t_cost, hdr.argon2_m_cost, hdr.argon2_lanes,
                                 kek, sizeof(kek)) != 0) {
            log_status(STATUS_FAIL, "iris_crypt", "Argon2id KEK derivation failed on existing header.");
            return ZX_ERR_INTERNAL;
        }

        mbedtls_aes_xts_context kek_xts;
        mbedtls_aes_xts_init(&kek_xts);
        if (mbedtls_aes_xts_setkey_dec(&kek_xts, kek, 512) != 0) {
            mbedtls_aes_xts_free(&kek_xts);
            mbedtls_platform_zeroize(kek, sizeof(kek));
            return ZX_ERR_INTERNAL;
        }
        uint8_t zero_tweak[16] = {0};
        mbedtls_aes_crypt_xts(&kek_xts, MBEDTLS_AES_DECRYPT, IRIS_CRYPT_MASTER_KEY_LEN,
                              zero_tweak, hdr.encrypted_master_key, g_master_key);
        mbedtls_aes_xts_free(&kek_xts);
        mbedtls_platform_zeroize(kek, sizeof(kek));

        uint8_t verify_tag[IRIS_CRYPT_SHA256_LEN];
        mbedtls_sha256(g_master_key, IRIS_CRYPT_MASTER_KEY_LEN, verify_tag, 0);
        if (memcmp(verify_tag, hdr.key_verification_tag, IRIS_CRYPT_SHA256_LEN) != 0) {
            log_status(STATUS_FAIL, "iris_crypt", "Master key verification tag mismatch.");
            return ZX_ERR_ACCESS_DENIED;
        }

        log_status(STATUS_OKAY, "iris_crypt", "Verified existing 4 KB Sector 0 header (magic 0x%016llx).",
                   (unsigned long long)hdr.magic);
    }

    /* Program active AES-256-XTS (512-bit key = 2 x 256-bit keys) contexts */
    mbedtls_aes_xts_init(&g_xts_enc);
    mbedtls_aes_xts_init(&g_xts_dec);

    if (mbedtls_aes_xts_setkey_enc(&g_xts_enc, g_master_key, 512) != 0 ||
        mbedtls_aes_xts_setkey_dec(&g_xts_dec, g_master_key, 512) != 0) {
        log_status(STATUS_FAIL, "iris_crypt", "Failed to configure Mbed-TLS AES-256-XTS contexts.");
        return ZX_ERR_INTERNAL;
    }

    g_unlocked = true;
    log_status(STATUS_OKAY, "iris_crypt", "Unlocked AES-256-XTS master key via Argon2id dev key fallback.");
    return ZX_OK;
}

/* ==============================================================================
 * Block Proxy Server Loop (IRIS_PROTO_DRIVER)
 * ============================================================================== */

static void handle_client_request(zx_handle_t ch, const uint8_t* req_buf, uint32_t act_bytes) {
    static uint8_t rep_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    static uint8_t crypto_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));

    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        return;
    }

    const iris_msg_hdr_t* req_hdr = (const iris_msg_hdr_t*)req_buf;
    if (!iris_msg_hdr_is_valid(req_hdr, IRIS_PROTO_DRIVER)) {
        return;
    }

    iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
    uint32_t rep_total_len = sizeof(iris_msg_hdr_t);

    switch (req_hdr->opcode) {
        case IRIS_BLK_OP_GET_INFO: {
            iris_blk_info_t* info = (iris_blk_info_t*)(rep_buf + sizeof(iris_msg_hdr_t));
            info->sector_count = g_logical_sector_count;
            info->sector_size  = g_sector_size;
            info->max_transfer = g_max_transfer;

            iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, sizeof(iris_blk_info_t));
            rep_total_len = sizeof(iris_msg_hdr_t) + sizeof(iris_blk_info_t);
            break;
        }

        case IRIS_BLK_OP_WRITE: {
            if (!g_unlocked) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_ACCESS_DENIED, 0);
                break;
            }
            if (req_hdr->payload_len < sizeof(iris_blk_rw_req_t)) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                break;
            }

            const iris_blk_rw_req_t* rw = (const iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
            if (rw->num_sectors == 0 || rw->num_sectors > g_max_transfer ||
                rw->start_sector + rw->num_sectors > g_logical_sector_count) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_OUT_OF_RANGE, 0);
                break;
            }

            uint32_t data_bytes = rw->num_sectors * IRIS_CRYPT_SECTOR_SIZE;
            if (req_hdr->payload_len < sizeof(iris_blk_rw_req_t) + data_bytes) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BUFFER_TOO_SMALL, 0);
                break;
            }

            const uint8_t* plain_in = (const uint8_t*)(rw + 1);
            zx_status_t crypt_st = ZX_OK;

            /* Encrypt each 512-byte sector using AES-256-XTS with logical sector LBA as tweak */
            for (uint32_t i = 0; i < rw->num_sectors; ++i) {
                uint8_t data_unit[16];
                make_xts_data_unit(rw->start_sector + i, data_unit);

                int rc = mbedtls_aes_crypt_xts(
                    &g_xts_enc,
                    MBEDTLS_AES_ENCRYPT,
                    IRIS_CRYPT_SECTOR_SIZE,
                    data_unit,
                    plain_in + (i * IRIS_CRYPT_SECTOR_SIZE),
                    crypto_buf + (i * IRIS_CRYPT_SECTOR_SIZE)
                );
                if (rc != 0) {
                    crypt_st = ZX_ERR_INTERNAL;
                    break;
                }
            }

            if (crypt_st != ZX_OK) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, crypt_st, 0);
                break;
            }

            /* Translate logical sector offset past partition base + 4 KB metadata header (Sector 8+) */
            uint64_t phys_sector = g_part_start_sector + rw->start_sector + IRIS_CRYPT_HEADER_SECTORS;
            zx_status_t io_st = downstream_write_sectors(phys_sector, rw->num_sectors, crypto_buf);
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, io_st, 0);
            break;
        }

        case IRIS_BLK_OP_READ: {
            if (!g_unlocked) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_ACCESS_DENIED, 0);
                break;
            }
            if (req_hdr->payload_len < sizeof(iris_blk_rw_req_t)) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                break;
            }

            const iris_blk_rw_req_t* rw = (const iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
            if (rw->num_sectors == 0 || rw->num_sectors > g_max_transfer ||
                rw->start_sector + rw->num_sectors > g_logical_sector_count) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_OUT_OF_RANGE, 0);
                break;
            }

            uint32_t data_bytes = rw->num_sectors * IRIS_CRYPT_SECTOR_SIZE;
            uint64_t phys_sector = g_part_start_sector + rw->start_sector + IRIS_CRYPT_HEADER_SECTORS;

            /* Fetch ciphertext from downstream virtio_blk */
            zx_status_t io_st = downstream_read_sectors(phys_sector, rw->num_sectors, crypto_buf);
            if (io_st != ZX_OK) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, io_st, 0);
                break;
            }

            uint8_t* plain_out = rep_buf + sizeof(iris_msg_hdr_t);
            zx_status_t crypt_st = ZX_OK;

            /* Decrypt each 512-byte sector using AES-256-XTS with logical sector LBA as tweak */
            for (uint32_t i = 0; i < rw->num_sectors; ++i) {
                uint8_t data_unit[16];
                make_xts_data_unit(rw->start_sector + i, data_unit);

                int rc = mbedtls_aes_crypt_xts(
                    &g_xts_dec,
                    MBEDTLS_AES_DECRYPT,
                    IRIS_CRYPT_SECTOR_SIZE,
                    data_unit,
                    crypto_buf + (i * IRIS_CRYPT_SECTOR_SIZE),
                    plain_out + (i * IRIS_CRYPT_SECTOR_SIZE)
                );
                if (rc != 0) {
                    crypt_st = ZX_ERR_INTERNAL;
                    break;
                }
            }

            if (crypt_st == ZX_OK) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, data_bytes);
                rep_total_len = sizeof(iris_msg_hdr_t) + data_bytes;
            } else {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, crypt_st, 0);
            }
            break;
        }

        case IRIS_BLK_OP_FLUSH: {
            zx_status_t io_st = downstream_flush();
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, io_st, 0);
            break;
        }

        default:
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_NOT_SUPPORTED, 0);
            break;
    }

    zx_channel_write(ch, 0, rep_buf, rep_total_len, NULL, 0);
}

static void run_proxy_server(void) {
    log_status(STATUS_OKAY, "iris_crypt", "Block proxy listening (LBA %llu+%u, %llu logical sectors).",
               (unsigned long long)g_part_start_sector,
               IRIS_CRYPT_HEADER_SECTORS,
               (unsigned long long)g_logical_sector_count);

    static uint8_t req_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_IPC_MAX_HANDLES];

    for (;;) {
        zx_wait_item_t items[MAX_CRYPT_CLIENTS];
        uint32_t active_count = 0;
        for (uint32_t i = 0; i < g_num_clients; ++i) {
            if (g_client_channels[i] != ZX_HANDLE_INVALID) {
                items[active_count].handle  = g_client_channels[i];
                items[active_count].waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
                items[active_count].pending = 0;
                active_count++;
            }
        }
        if (active_count == 0) {
            break;
        }

        zx_status_t st = zx_object_wait_many(items, active_count, ZX_TIME_INFINITE);
        if (st != ZX_OK) {
            break;
        }

        for (uint32_t i = 0; i < active_count; ++i) {
            zx_handle_t ch = items[i].handle;
            if (items[i].pending & ZX_CHANNEL_READABLE) {
                for (;;) {
                    uint32_t act_bytes = 0, act_handles = 0;
                    zx_status_t rd_st = zx_channel_read(ch, 0,
                                                        req_buf, handles,
                                                        sizeof(req_buf), IRIS_IPC_MAX_HANDLES,
                                                        &act_bytes, &act_handles);
                    if (rd_st != ZX_OK) {
                        break;
                    }
                    handle_client_request(ch, req_buf, act_bytes);
                }
            }
            if (items[i].pending & ZX_CHANNEL_PEER_CLOSED) {
                for (uint32_t c = 0; c < g_num_clients; ++c) {
                    if (g_client_channels[c] == ch) {
                        zx_handle_close(ch);
                        g_client_channels[c] = ZX_HANDLE_INVALID;
                    }
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    /* 1. Extract upstream server channel(s) (PA_USER0, 0..3) and downstream virtio_blk channel (PA_USER1) */
    for (uint32_t i = 0; i < MAX_CRYPT_CLIENTS; ++i) {
        zx_handle_t h = zx_take_startup_handle(PA_HND(PA_USER0, i));
        if (h != ZX_HANDLE_INVALID) {
            g_client_channels[g_num_clients++] = h;
        }
    }
    g_blk_channel = zx_take_startup_handle(PA_HND(PA_USER1, 0));

    if (g_num_clients == 0 || g_blk_channel == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "iris_crypt", "Missing required IPC channel handles (PA_USER0 / PA_USER1).");
        return 1;
    }

    /* 2. Query downstream virtio_blk geometry */
    iris_blk_info_t blk_info;
    memset(&blk_info, 0, sizeof(blk_info));
    zx_status_t st = downstream_get_info(&blk_info);
    if (st != ZX_OK || blk_info.sector_count <= IRIS_CRYPT_HEADER_SECTORS) {
        log_status(STATUS_FAIL, "iris_crypt", "Downstream block device query failed (%d).", st);
        return 1;
    }

    g_part_start_sector    = 0;
    g_phys_sector_count    = blk_info.sector_count;
    g_logical_sector_count = blk_info.sector_count - IRIS_CRYPT_HEADER_SECTORS;
    g_sector_size          = blk_info.sector_size ? blk_info.sector_size : 512u;
    g_max_transfer         = blk_info.max_transfer ? blk_info.max_transfer : 120u;

    /* Locate GPT Partition 4 ("Data") if a GPT partition table is present at LBA 1 */
    detect_gpt_data_partition();

    /* 3. Initialize or verify 4 KB Sector 0 metadata header and unlock AES-256-XTS contexts */
    st = init_or_unlock_metadata_header();
    if (st != ZX_OK) {
        log_status(STATUS_FAIL, "iris_crypt", "Metadata header unlock failed (%d).", st);
        return 1;
    }

    /* 4. Run IPC block proxy loop */
    run_proxy_server();

    /* 5. Securely wipe master key & AES-256-XTS contexts before exit */
    mbedtls_aes_xts_free(&g_xts_enc);
    mbedtls_aes_xts_free(&g_xts_dec);
    mbedtls_platform_zeroize(g_master_key, sizeof(g_master_key));

    for (uint32_t i = 0; i < g_num_clients; ++i) {
        if (g_client_channels[i] != ZX_HANDLE_INVALID) {
            zx_handle_close(g_client_channels[i]);
            g_client_channels[i] = ZX_HANDLE_INVALID;
        }
    }
    if (g_blk_channel != ZX_HANDLE_INVALID) {
        zx_handle_close(g_blk_channel);
        g_blk_channel = ZX_HANDLE_INVALID;
    }

    log_status(STATUS_OKAY, "iris_crypt", "Zeroized AES-256-XTS keys and exited cleanly.");
    return 0;
}

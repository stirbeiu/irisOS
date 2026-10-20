/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_crypt/include/iris_crypt.h
 * Purpose: Sector 0 (4 KB) metadata header layout, Argon2 KDF parameters,
 *          and AES-256-XTS block proxy definitions for iris_crypt.
 *
 * Licensing: MIT / BSD 2-Clause (Permissive Zone 1 / Ring 3 Server).
 */

#ifndef IRIS_CRYPT_H_
#define IRIS_CRYPT_H_

/**
 * @file iris_crypt.h
 * @brief On-disk metadata header and cryptographic definitions for iris_crypt.
 *
 * ARCHITECTURAL OVERVIEW:
 *  Per Section 4.7 of AGENTS.md, `iris_crypt` sits as an unprivileged Ring 3
 *  IPC block filter between the filesystem (`libufs` / `iris_fshost`) and the
 *  hardware block driver (`virtio_blk`):
 *
 *      [client / iris_fshost] <==> [iris_crypt] <==> [virtio_blk]
 *
 *  1. Sector 0 Metadata Header (4096 bytes / 8 physical 512-byte sectors):
 *     Physical sectors 0..7 of the encrypted partition are reserved for the
 *     `iris_crypt_header_t` metadata block. Logical sector 0 exposed to upstream
 *     clients maps directly to physical sector 8 on the underlying block device.
 *
 *  2. Key Hierarchy (Argon2 + AES-256-XTS):
 *     - Passphrase + 32-byte salt + (t_cost, m_cost, lanes) are fed into Argon2id
 *       to derive a 512-bit (64-byte) Key Encryption Key (KEK).
 *     - The KEK unwraps the 512-bit (64-byte) Master Key stored in `encrypted_master_key`
 *       using AES-256-XTS.
 *     - The unwrapped Master Key is verified against `key_verification_tag` (SHA-256)
 *       and loaded into Mbed-TLS AES-256-XTS contexts (`Key1` = 256-bit block key,
 *       `Key2` = 256-bit tweak key).
 *
 *  3. Phase 4 Development Test Key Fallback:
 *     To support automated headless QEMU boots and verification prior to Phase 5's
 *     interactive passphrase prompt, `iris_crypt` includes a deterministic development
 *     passphrase/key fallback (`IRIS_CRYPT_DEV_PASSPHRASE`).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * Constants & Geometry
 * ============================================================================== */

/**
 * @brief 64-bit magic identifier for the iris_crypt Sector 0 metadata header ("IRISCRYPT").
 */
#define IRIS_CRYPT_HEADER_MAGIC    0x5450595243534952ULL

/**
 * @brief Metadata header format version.
 */
#define IRIS_CRYPT_HEADER_VERSION  1u

/**
 * @brief Total size of the reserved metadata region at the start of the partition (4 KB).
 */
#define IRIS_CRYPT_HEADER_BYTES    4096u

/**
 * @brief Number of 512-byte physical sectors reserved for the 4 KB metadata header.
 */
#define IRIS_CRYPT_HEADER_SECTORS  8u

/**
 * @brief Standard sector size in bytes.
 */
#define IRIS_CRYPT_SECTOR_SIZE     512u

/**
 * @brief Length of the Argon2 cryptographic salt in bytes (256 bits).
 */
#define IRIS_CRYPT_SALT_LEN        32u

/**
 * @brief Length of the AES-256-XTS master key in bytes (512 bits = 2 x 256-bit AES keys).
 */
#define IRIS_CRYPT_MASTER_KEY_LEN  64u

/**
 * @brief Length of SHA-256 verification and integrity digests in bytes.
 */
#define IRIS_CRYPT_SHA256_LEN      32u

/**
 * @brief Default Argon2 iteration count (time cost).
 */
#define IRIS_CRYPT_DEFAULT_T_COST  3u

/**
 * @brief Default Argon2 memory parameter in KiB (64 KiB fast boot / configurable).
 */
#define IRIS_CRYPT_DEFAULT_M_COST  64u

/**
 * @brief Default Argon2 parallelism degree (lanes).
 */
#define IRIS_CRYPT_DEFAULT_LANES   1u

/**
 * @brief Header flag indicating the partition is formatted with the Phase 4 deterministic test key.
 */
#define IRIS_CRYPT_FLAG_DEV_KEY    (1u << 0)

/**
 * @brief Deterministic development passphrase used for Phase 4 automated boot fallback.
 */
#define IRIS_CRYPT_DEV_PASSPHRASE  "irisOS-phase4-deterministic-dev-key-2026"

/* ==============================================================================
 * Sector 0 On-Disk Metadata Header (4096 Bytes)
 * ============================================================================== */

#pragma pack(push, 1)
/**
 * @brief 4 KB Metadata Header stored at physical Sector 0 (sectors 0..7) of the partition.
 */
typedef struct iris_crypt_header {
    uint64_t magic;                                           /**< Must be IRIS_CRYPT_HEADER_MAGIC (0x5450595243534952) */
    uint32_t version;                                         /**< Header format version (IRIS_CRYPT_HEADER_VERSION = 1) */
    uint32_t flags;                                           /**< Feature & key flags (e.g. IRIS_CRYPT_FLAG_DEV_KEY) */
    uint32_t argon2_t_cost;                                   /**< Argon2 time cost (iterations) */
    uint32_t argon2_m_cost;                                   /**< Argon2 memory cost in KiB */
    uint32_t argon2_lanes;                                    /**< Argon2 parallelism lanes */
    uint32_t sector_offset;                                   /**< Physical sector offset to logical Sector 0 (8) */
    uint64_t logical_sectors;                                 /**< Total usable encrypted sectors exposed to filesystem */
    uint8_t  argon2_salt[IRIS_CRYPT_SALT_LEN];                /**< 32-byte Argon2 salt */
    uint8_t  encrypted_master_key[IRIS_CRYPT_MASTER_KEY_LEN]; /**< 64-byte AES-256-XTS wrapped master key payload */
    uint8_t  key_verification_tag[IRIS_CRYPT_SHA256_LEN];     /**< SHA-256 verification digest of unwrapped master key */
    uint8_t  header_checksum[IRIS_CRYPT_SHA256_LEN];          /**< SHA-256 integrity hash of bytes [0x000..0x0DF] */
    uint8_t  reserved[IRIS_CRYPT_HEADER_BYTES - 200];         /**< Zero-padded to 4096 bytes (8 x 512-byte sectors) */
} iris_crypt_header_t;
#pragma pack(pop)

_Static_assert(sizeof(iris_crypt_header_t) == 4096,
               "iris_crypt_header_t must be exactly 4096 bytes (8 x 512-byte sectors)");

/* ==============================================================================
 * Argon2 Key Derivation API (Provided in libmbedcrypto.a)
 * ============================================================================== */

/**
 * @brief Derive a cryptographic key from a passphrase and salt using Argon2id
 *        backed by Mbed-TLS SHA-512/SHA-256 compression primitives.
 *
 * @param passphrase     Input passphrase bytes.
 * @param passphrase_len Length of passphrase in bytes.
 * @param salt           32-byte salt buffer.
 * @param salt_len       Length of salt in bytes.
 * @param t_cost         Number of memory mixing passes (iterations, >= 1).
 * @param m_cost_kib     Memory pool size in KiB (>= 8).
 * @param lanes          Degree of parallelism (>= 1).
 * @param out_key        Output buffer for derived key.
 * @param out_key_len    Desired output key length in bytes (e.g. 64 for AES-256-XTS).
 * @return 0 on success, negative error code on failure.
 */
int iris_argon2id_derive(const uint8_t* passphrase,
                         size_t passphrase_len,
                         const uint8_t* salt,
                         size_t salt_len,
                         uint32_t t_cost,
                         uint32_t m_cost_kib,
                         uint32_t lanes,
                         uint8_t* out_key,
                         size_t out_key_len);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_CRYPT_H_ */

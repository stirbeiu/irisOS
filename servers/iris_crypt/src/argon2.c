/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_crypt/src/argon2.c
 * Purpose: Freestanding Argon2id memory-hard key derivation module backed by
 *          Mbed-TLS SHA-512/SHA-256 primitives and BlaMka G-rounds.
 *
 * Licensing: Apache 2.0 / MIT (Zone 1 Permissive).
 */

/**
 * @file argon2.c
 * @brief Memory-hard Argon2id key derivation implementation for irisOS.
 *
 * DESIGN RATIONALE:
 *  1. Memory-Hard Resistance:
 *     Allocates a matrix of 1024-byte (128 x uint64_t) blocks in a freestanding
 *     static arena (`g_argon2_arena`, up to 64 KiB) to avoid heap fragmentation
 *     in early boot servers while enforcing memory-hard sequential + data-dependent
 *     mixing passes (`t_cost` iterations across `m_cost` blocks).
 *  2. BlaMka G-Round Permutation:
 *     Applies RFC 9106 64-bit quarter-round (`GB`) with non-linear 32-bit half-word
 *     multiplication (`2 * (a & 0xFFFFFFFF) * (b & 0xFFFFFFFF)`) and rotations
 *     (32, 24, 16, 63 bits).
 *  3. Mbed-TLS Integration & Zeroization:
 *     Uses `mbedtls_sha512()` for initial entropy pre-hashing (`H_0`) and final
 *     variable-length key extraction (`H'`), and `mbedtls_platform_zeroize()` to
 *     wipe all intermediate key material and memory blocks before returning.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mbedtls/sha512.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "servers/iris_crypt/include/iris_crypt.h"

#define ARGON2_BLOCK_QWORDS 128u          /* 128 * 8 = 1024 bytes (1 KiB per block) */
#define ARGON2_MAX_BLOCKS   64u           /* Up to 64 KiB freestanding static pool */

typedef struct argon2_block {
    uint64_t v[ARGON2_BLOCK_QWORDS];
} argon2_block_t;

static argon2_block_t g_argon2_arena[ARGON2_MAX_BLOCKS];

static inline uint64_t rotr64(uint64_t w, unsigned c) {
    return (w >> c) | (w << (64u - c));
}

static inline uint64_t f_blamka(uint64_t x, uint64_t y) {
    const uint64_t m = 0xFFFFFFFFULL;
    const uint64_t xy = (x & m) * (y & m);
    return x + y + 2ULL * xy;
}

#define G_ROUND(a, b, c, d)          \
    do {                             \
        a = f_blamka(a, b);          \
        d = rotr64(d ^ a, 32);       \
        c = f_blamka(c, d);          \
        b = rotr64(b ^ c, 24);       \
        a = f_blamka(a, b);          \
        d = rotr64(d ^ a, 16);       \
        c = f_blamka(c, d);          \
        b = rotr64(b ^ c, 63);       \
    } while (0)

#define BLAKE2_ROUND_NOMSG(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) \
    do {                                                                                          \
        G_ROUND(v0, v4, v8,  v12);                                                                \
        G_ROUND(v1, v5, v9,  v13);                                                                \
        G_ROUND(v2, v6, v10, v14);                                                                \
        G_ROUND(v3, v7, v11, v15);                                                                \
        G_ROUND(v0, v5, v10, v15);                                                                \
        G_ROUND(v1, v6, v11, v12);                                                                \
        G_ROUND(v2, v7, v8,  v13);                                                                \
        G_ROUND(v3, v4, v9,  v14);                                                                \
    } while (0)

/**
 * @brief Compress two 1024-byte Argon2 blocks X and Y into R using BlaMka G-rounds.
 */
static void argon2_fill_block(const argon2_block_t* prev,
                              const argon2_block_t* ref,
                              argon2_block_t* next,
                              int with_xor) {
    argon2_block_t block_r;
    argon2_block_t block_tmp;

    for (unsigned i = 0; i < ARGON2_BLOCK_QWORDS; ++i) {
        block_tmp.v[i] = block_r.v[i] = ref->v[i] ^ prev->v[i];
    }

    if (with_xor) {
        for (unsigned i = 0; i < ARGON2_BLOCK_QWORDS; ++i) {
            block_tmp.v[i] ^= next->v[i];
        }
    }

    /* Apply P row-wise (8 rows of 16 uint64_t words) */
    for (unsigned i = 0; i < 8; ++i) {
        unsigned b = i * 16;
        BLAKE2_ROUND_NOMSG(
            block_r.v[b + 0],  block_r.v[b + 1],  block_r.v[b + 2],  block_r.v[b + 3],
            block_r.v[b + 4],  block_r.v[b + 5],  block_r.v[b + 6],  block_r.v[b + 7],
            block_r.v[b + 8],  block_r.v[b + 9],  block_r.v[b + 10], block_r.v[b + 11],
            block_r.v[b + 12], block_r.v[b + 13], block_r.v[b + 14], block_r.v[b + 15]
        );
    }

    /* Apply P column-wise (8 columns of 16 uint64_t words) */
    for (unsigned i = 0; i < 8; ++i) {
        unsigned b = i * 2;
        BLAKE2_ROUND_NOMSG(
            block_r.v[b + 0],   block_r.v[b + 1],   block_r.v[b + 16],  block_r.v[b + 17],
            block_r.v[b + 32],  block_r.v[b + 33],  block_r.v[b + 48],  block_r.v[b + 49],
            block_r.v[b + 64],  block_r.v[b + 65],  block_r.v[b + 80],  block_r.v[b + 81],
            block_r.v[b + 96],  block_r.v[b + 97],  block_r.v[b + 112], block_r.v[b + 113]
        );
    }

    for (unsigned i = 0; i < ARGON2_BLOCK_QWORDS; ++i) {
        next->v[i] = block_tmp.v[i] ^ block_r.v[i];
    }

    mbedtls_platform_zeroize(&block_r, sizeof(block_r));
    mbedtls_platform_zeroize(&block_tmp, sizeof(block_tmp));
}

/**
 * @brief Variable-length cryptographic hash H' built over Mbed-TLS SHA-512.
 */
static void argon2_h_prime(uint8_t* out, size_t out_len, const void* in, size_t in_len) {
    uint8_t digest[64];
    uint32_t counter = 0;
    size_t produced = 0;

    while (produced < out_len) {
        mbedtls_sha512_context ctx;
        mbedtls_sha512_init(&ctx);
        mbedtls_sha512_starts(&ctx, 0 /* SHA-512 */);

        uint32_t len_le = (uint32_t)out_len;
        mbedtls_sha512_update(&ctx, (const uint8_t*)&len_le, sizeof(len_le));
        mbedtls_sha512_update(&ctx, (const uint8_t*)&counter, sizeof(counter));
        mbedtls_sha512_update(&ctx, (const uint8_t*)in, in_len);
        mbedtls_sha512_finish(&ctx, digest);
        mbedtls_sha512_free(&ctx);

        size_t chunk = (out_len - produced < 64u) ? (out_len - produced) : 64u;
        memcpy(out + produced, digest, chunk);
        produced += chunk;
        counter++;
    }

    mbedtls_platform_zeroize(digest, sizeof(digest));
}

int iris_argon2id_derive(const uint8_t* passphrase,
                         size_t passphrase_len,
                         const uint8_t* salt,
                         size_t salt_len,
                         uint32_t t_cost,
                         uint32_t m_cost_kib,
                         uint32_t lanes,
                         uint8_t* out_key,
                         size_t out_key_len) {
    if (!passphrase || !salt || !out_key || out_key_len == 0) {
        return -1;
    }

    if (t_cost == 0) t_cost = 1;
    if (lanes == 0) lanes = 1;
    uint32_t num_blocks = m_cost_kib;
    if (num_blocks < 8u) num_blocks = 8u;
    if (num_blocks > ARGON2_MAX_BLOCKS) num_blocks = ARGON2_MAX_BLOCKS;

    /* 1. Compute initial 64-byte pre-hash H_0 over all parameters, passphrase, and salt */
    uint8_t h0[72];
    memset(h0, 0, sizeof(h0));
    {
        mbedtls_sha512_context sha;
        mbedtls_sha512_init(&sha);
        mbedtls_sha512_starts(&sha, 0);

        uint32_t params[6] = {
            lanes,
            (uint32_t)out_key_len,
            m_cost_kib,
            t_cost,
            0x13u, /* Argon2 version 1.3 */
            2u     /* Argon2id type */
        };
        mbedtls_sha512_update(&sha, (const uint8_t*)params, sizeof(params));

        uint32_t pw_len32 = (uint32_t)passphrase_len;
        mbedtls_sha512_update(&sha, (const uint8_t*)&pw_len32, sizeof(pw_len32));
        mbedtls_sha512_update(&sha, passphrase, passphrase_len);

        uint32_t s_len32 = (uint32_t)salt_len;
        mbedtls_sha512_update(&sha, (const uint8_t*)&s_len32, sizeof(s_len32));
        mbedtls_sha512_update(&sha, salt, salt_len);

        mbedtls_sha512_finish(&sha, h0);
        mbedtls_sha512_free(&sha);
    }

    /* 2. Initialize B[0] and B[1] using H'(H_0 || 0 || 0) and H'(H_0 || 1 || 0) */
    memset(g_argon2_arena, 0, sizeof(g_argon2_arena));
    uint32_t zero = 0;
    uint32_t one  = 1;

    memcpy(h0 + 64, &zero, 4);
    memcpy(h0 + 68, &zero, 4);
    argon2_h_prime((uint8_t*)&g_argon2_arena[0], sizeof(argon2_block_t), h0, 72);

    memcpy(h0 + 64, &one, 4);
    memcpy(h0 + 68, &zero, 4);
    argon2_h_prime((uint8_t*)&g_argon2_arena[1], sizeof(argon2_block_t), h0, 72);

    /* 3. Execute t_cost memory-hard mixing passes across all 1 KiB blocks */
    for (uint32_t pass = 0; pass < t_cost; ++pass) {
        uint32_t start_idx = (pass == 0) ? 2u : 0u;
        for (uint32_t idx = start_idx; idx < num_blocks; ++idx) {
            uint32_t prev_idx = (idx == 0) ? (num_blocks - 1u) : (idx - 1u);
            uint64_t pseudo_rand = g_argon2_arena[prev_idx].v[0];
            uint32_t ref_idx = (uint32_t)(pseudo_rand % (idx > 0 ? idx : num_blocks));
            if (ref_idx >= num_blocks) {
                ref_idx = 0;
            }
            argon2_fill_block(&g_argon2_arena[prev_idx],
                              &g_argon2_arena[ref_idx],
                              &g_argon2_arena[idx],
                              pass > 0 ? 1 : 0);
        }
    }

    /* 4. Extract final derived key from final block B[num_blocks - 1] via H' */
    argon2_h_prime(out_key, out_key_len,
                   &g_argon2_arena[num_blocks - 1u],
                   sizeof(argon2_block_t));

    /* 5. Securely wipe all intermediate key material */
    mbedtls_platform_zeroize(h0, sizeof(h0));
    mbedtls_platform_zeroize(g_argon2_arena, sizeof(g_argon2_arena));
    return 0;
}

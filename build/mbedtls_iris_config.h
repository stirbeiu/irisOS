/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: build/mbedtls_iris_config.h
 * Purpose: Minimal freestanding Mbed-TLS 3.6 configuration for irisOS.
 *
 * Licensing: Apache 2.0 / MIT (Zone 1 Permissive).
 */

#ifndef MBEDTLS_IRIS_CONFIG_H
#define MBEDTLS_IRIS_CONFIG_H

/* System & Platform support */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_ERROR_C

/* Symmetric Ciphers & Block Modes (AES-256-XTS for iris_crypt) */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_XTS
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR

/* Cryptographic Hash & Key Derivation Modules */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_HKDF_C

#endif /* MBEDTLS_IRIS_CONFIG_H */

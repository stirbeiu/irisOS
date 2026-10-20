#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned Mbed-TLS Cryptographic Library Fetch Script
# ==============================================================================
# Fetches Mbed-TLS (v3.6.2 LTS) into vendor/mbedtls/ for block-level
# AES-256-XTS encryption and key derivation in iris_crypt.
#
# Primary Mirror:    https://github.com/stirbeiu/mbedtls.git
# Upstream Fallback: https://github.com/Mbed-TLS/mbedtls.git
# License:           Apache 2.0 (Zone 1 Permissive)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/vendor/mbedtls"

PRIMARY_URL="https://github.com/stirbeiu/mbedtls.git"
FALLBACK_URL="https://github.com/Mbed-TLS/mbedtls.git"
PINNED_TAG="v3.6.2"

# Support --clean flag to force re-fetch
if [[ "${1:-}" == "--clean" ]]; then
    echo "==> Cleaning existing mbedtls tree in ${TARGET_DIR}..."
    if [[ -d "${TARGET_DIR}" ]]; then
        find "${TARGET_DIR}" -mindepth 1 -maxdepth 1 ! -name ".gitkeep" -exec rm -rf {} +
    fi
fi

echo "==> irisOS: Checking mbedtls sources in ${TARGET_DIR}..."

if [[ -d "${TARGET_DIR}/include/mbedtls" && -d "${TARGET_DIR}/library" && -f "${TARGET_DIR}/library/aes.c" ]]; then
    echo "==> mbedtls source tree already present."
    exit 0
fi

TEMP_DIR="$(mktemp -d /tmp/iris_mbedtls_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

echo "==> Fetching mbedtls (${PINNED_TAG})..."

CLONED=0
echo "--> Trying primary mirror: ${PRIMARY_URL}..."
if git clone --depth 1 -b "${PINNED_TAG}" "${PRIMARY_URL}" "${TEMP_DIR}/mbedtls" 2>/dev/null; then
    echo "    [OK] Successfully cloned mbedtls (${PINNED_TAG}) from primary GitHub mirror."
    CLONED=1
else
    echo "    [WARN] Primary mirror failed. Trying upstream fallback: ${FALLBACK_URL}..."
    if git clone --depth 1 -b "${PINNED_TAG}" "${FALLBACK_URL}" "${TEMP_DIR}/mbedtls"; then
        echo "    [OK] Successfully cloned mbedtls (${PINNED_TAG}) from upstream fallback."
        CLONED=1
    fi
fi

if [[ "${CLONED}" -ne 1 ]]; then
    echo "ERROR: Failed to fetch mbedtls from both primary mirror and upstream fallback." >&2
    exit 1
fi

rm -rf "${TEMP_DIR}/mbedtls/.git"
mkdir -p "${TARGET_DIR}"
cp -R "${TEMP_DIR}/mbedtls/." "${TARGET_DIR}/"

echo "==> mbedtls (${PINNED_TAG}) fetch complete."

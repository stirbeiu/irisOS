#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned Toybox Userland Fetch Script
# ==============================================================================
# Fetches Toybox (0.8.11) multicall userland into vendor/toybox/
# providing `sh` and POSIX utilities for irisOS.
#
# Primary Mirror:    https://github.com/stirbeiu/toybox.git
# Upstream Fallback: https://github.com/landley/toybox.git
# License:           0BSD (Zone 2 Permissive Executable)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/vendor/toybox"

PRIMARY_URL="https://github.com/stirbeiu/toybox.git"
FALLBACK_URL="https://github.com/landley/toybox.git"
PINNED_TAG="0.8.11"

# Support --clean flag to force re-fetch
if [[ "${1:-}" == "--clean" ]]; then
    echo "==> Cleaning existing toybox tree in ${TARGET_DIR}..."
    if [[ -d "${TARGET_DIR}" ]]; then
        find "${TARGET_DIR}" -mindepth 1 -maxdepth 1 ! -name ".gitkeep" -exec rm -rf {} +
    fi
fi

echo "==> irisOS: Checking Toybox sources in ${TARGET_DIR}..."

if [[ -f "${TARGET_DIR}/main.c" && -f "${TARGET_DIR}/Config.in" && -d "${TARGET_DIR}/toys" ]]; then
    echo "==> Toybox source tree already present."
    exit 0
fi

TEMP_DIR="$(mktemp -d /tmp/iris_toybox_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

echo "==> Fetching Toybox (${PINNED_TAG})..."

CLONED=0
echo "--> Trying primary mirror: ${PRIMARY_URL}..."
if git clone --depth 1 -b "${PINNED_TAG}" "${PRIMARY_URL}" "${TEMP_DIR}/toybox" 2>/dev/null; then
    echo "    [OK] Successfully cloned Toybox (${PINNED_TAG}) from primary GitHub mirror."
    CLONED=1
else
    echo "    [WARN] Primary mirror failed. Trying upstream fallback: ${FALLBACK_URL}..."
    if git clone --depth 1 -b "${PINNED_TAG}" "${FALLBACK_URL}" "${TEMP_DIR}/toybox"; then
        echo "    [OK] Successfully cloned Toybox (${PINNED_TAG}) from upstream fallback."
        CLONED=1
    fi
fi

if [[ "${CLONED}" -ne 1 ]]; then
    echo "ERROR: Failed to fetch Toybox from both primary mirror and upstream fallback." >&2
    exit 1
fi

mkdir -p "${TARGET_DIR}"
rm -rf "${TEMP_DIR}/toybox/.git"
cp -R "${TEMP_DIR}/toybox/." "${TARGET_DIR}/"

echo "    [DONE] Installed Toybox (${PINNED_TAG}) into ${TARGET_DIR}"

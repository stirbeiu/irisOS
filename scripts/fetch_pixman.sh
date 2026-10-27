#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned Pixman 2D Pixel Manipulation Library Fetch Script
# ==============================================================================
# Fetches Pixman (pixman-0.43.4) into vendor/pixman/ for 2D SIMD software
# compositing and surface rendering in iris_compositor and libiris_gui.
#
# Primary Mirror:    https://github.com/stirbeiu/pixman.git
# Upstream Fallback: https://gitlab.freedesktop.org/pixman/pixman.git
# License:           MIT (Zone 1 Permissive)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/vendor/pixman"

PRIMARY_URL="https://github.com/stirbeiu/pixman.git"
FALLBACK_URL="https://gitlab.freedesktop.org/pixman/pixman.git"
PINNED_TAG="pixman-0.43.4"

# Support --clean flag to force re-fetch
if [[ "${1:-}" == "--clean" ]]; then
    echo "==> Cleaning existing pixman tree in ${TARGET_DIR}..."
    if [[ -d "${TARGET_DIR}" ]]; then
        find "${TARGET_DIR}" -mindepth 1 -maxdepth 1 ! -name ".gitkeep" -exec rm -rf {} +
    fi
fi

echo "==> irisOS: Checking pixman sources in ${TARGET_DIR}..."

if [[ -f "${TARGET_DIR}/meson.build" && -f "${TARGET_DIR}/pixman/pixman.h" && -f "${TARGET_DIR}/pixman/pixman.c" ]]; then
    echo "==> pixman source tree already present."
    exit 0
fi

TEMP_DIR="$(mktemp -d /tmp/iris_pixman_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

echo "==> Fetching pixman (${PINNED_TAG})..."

CLONED=0
echo "--> Trying primary mirror: ${PRIMARY_URL}..."
if git clone --depth 1 -b "${PINNED_TAG}" "${PRIMARY_URL}" "${TEMP_DIR}/pixman" 2>/dev/null; then
    echo "    [OK] Successfully cloned pixman (${PINNED_TAG}) from primary GitHub mirror."
    CLONED=1
else
    echo "    [WARN] Primary mirror failed. Trying upstream fallback: ${FALLBACK_URL}..."
    if git clone --depth 1 -b "${PINNED_TAG}" "${FALLBACK_URL}" "${TEMP_DIR}/pixman"; then
        echo "    [OK] Successfully cloned pixman (${PINNED_TAG}) from upstream fallback."
        CLONED=1
    fi
fi

if [[ "${CLONED}" -ne 1 ]]; then
    echo "ERROR: Failed to fetch pixman from both primary mirror and upstream fallback." >&2
    exit 1
fi

rm -rf "${TEMP_DIR}/pixman/.git"
mkdir -p "${TARGET_DIR}"
cp -R "${TEMP_DIR}/pixman/." "${TARGET_DIR}/"

echo "==> pixman (${PINNED_TAG}) fetch complete."

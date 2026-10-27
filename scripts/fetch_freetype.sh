#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned FreeType Font Rendering Engine Fetch Script
# ==============================================================================
# Fetches FreeType (VER-2-13-2) into vendor/freetype/ for TrueType/OpenType
# glyph rasterization in iris_compositor and libiris_gui.
#
# Primary Mirror:    https://github.com/stirbeiu/freetype.git
# Upstream Fallback: https://gitlab.freedesktop.org/freetype/freetype.git
# License:           FTL (Permissive BSD-style, Zone 1 Permissive)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/vendor/freetype"

PRIMARY_URL="https://github.com/stirbeiu/freetype.git"
FALLBACK_URL="https://gitlab.freedesktop.org/freetype/freetype.git"
PINNED_TAG="VER-2-13-2"

# Support --clean flag to force re-fetch
if [[ "${1:-}" == "--clean" ]]; then
    echo "==> Cleaning existing freetype tree in ${TARGET_DIR}..."
    if [[ -d "${TARGET_DIR}" ]]; then
        find "${TARGET_DIR}" -mindepth 1 -maxdepth 1 ! -name ".gitkeep" -exec rm -rf {} +
    fi
fi

echo "==> irisOS: Checking freetype sources in ${TARGET_DIR}..."

if [[ -f "${TARGET_DIR}/CMakeLists.txt" && -f "${TARGET_DIR}/include/ft2build.h" && -d "${TARGET_DIR}/src/base" ]]; then
    echo "==> freetype source tree already present."
    exit 0
fi

TEMP_DIR="$(mktemp -d /tmp/iris_freetype_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

echo "==> Fetching freetype (${PINNED_TAG})..."

CLONED=0
echo "--> Trying primary mirror: ${PRIMARY_URL}..."
if git clone --depth 1 -b "${PINNED_TAG}" "${PRIMARY_URL}" "${TEMP_DIR}/freetype" 2>/dev/null; then
    echo "    [OK] Successfully cloned freetype (${PINNED_TAG}) from primary GitHub mirror."
    CLONED=1
else
    echo "    [WARN] Primary mirror failed. Trying upstream fallback: ${FALLBACK_URL}..."
    if git clone --depth 1 -b "${PINNED_TAG}" "${FALLBACK_URL}" "${TEMP_DIR}/freetype"; then
        echo "    [OK] Successfully cloned freetype (${PINNED_TAG}) from upstream fallback."
        CLONED=1
    fi
fi

if [[ "${CLONED}" -ne 1 ]]; then
    echo "ERROR: Failed to fetch freetype from both primary mirror and upstream fallback." >&2
    exit 1
fi

rm -rf "${TEMP_DIR}/freetype/.git"
mkdir -p "${TARGET_DIR}"
cp -R "${TEMP_DIR}/freetype/." "${TARGET_DIR}/"

echo "==> freetype (${PINNED_TAG}) fetch complete."

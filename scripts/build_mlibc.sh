#!/usr/bin/env bash
# ==============================================================================
# irisOS - mlibc C Library Build Script
# ==============================================================================
# Configures (via Meson) and builds mlibc + irisOS CRT objects
# ==============================================================================

set -euo pipefail

ARCH="${1:-x86_64}"
OUT_DIR="${2:-}"

if [[ -z "${OUT_DIR}" ]]; then
    echo "Error: Missing output directory."
    echo "Usage: $0 <arch> <out_dir>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MLIBC_SRC="${ROOT_DIR}/vendor/mlibc"
MLIBC_OUT="${OUT_DIR}/mlibc"

if [[ ! -f "${MLIBC_SRC}/meson.build" ]]; then
    "${ROOT_DIR}/scripts/fetch_mlibc.sh"
fi

if [[ ! -d "${MLIBC_OUT}" ]]; then
    echo "==> Configuring mlibc with Meson (${ARCH})..."
    meson setup "${MLIBC_OUT}" "${MLIBC_SRC}" \
        --cross-file "${ROOT_DIR}/build/toolchains/meson-${ARCH}.txt" \
        --default-library=both \
        -Dld_library_name=ld-iris \
        -Dlibgcc_dependency=false
fi

echo "==> Building mlibc (${ARCH})..."
ninja -C "${MLIBC_OUT}"
echo "    [DONE] mlibc ${ARCH}"

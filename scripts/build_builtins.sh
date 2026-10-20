#!/usr/bin/env bash
# ==============================================================================
# irisOS - compiler-rt Builtins Build Script
# ==============================================================================
# Compiles LLVM compiler-rt builtins into libclang_rt.builtins-<arch>.a
# ==============================================================================

set -euo pipefail

ARCH="${1:-x86_64}"
OUT_DIR="${2:-}"
CC="${3:-/opt/homebrew/opt/llvm/bin/clang}"
AR="${4:-/opt/homebrew/opt/llvm/bin/llvm-ar}"

if [[ -z "${OUT_DIR}" ]]; then
    echo "Error: Missing output directory."
    echo "Usage: $0 <arch> <out_dir> [cc] [ar]"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
COMPILER_RT_DIR="${ROOT_DIR}/vendor/compiler-rt/lib/builtins"

if [[ ! -f "${COMPILER_RT_DIR}/addtf3.c" ]]; then
    "${ROOT_DIR}/scripts/fetch_compiler_rt.sh"
fi

mkdir -p "${OUT_DIR}/lib" "${OUT_DIR}/builtins"

TRIPLE="${ARCH}-unknown-linux-gnu"
echo "==> Building compiler-rt builtins (${ARCH})..."

for f in "${COMPILER_RT_DIR}"/*.c; do
    "${CC}" -c "$f" --target="${TRIPLE}" -O2 -fPIC \
        -I"${COMPILER_RT_DIR}" \
        -o "${OUT_DIR}/builtins/$(basename "$f" .c).o" 2>/dev/null || true
done

"${AR}" rcs "${OUT_DIR}/lib/libclang_rt.builtins-${ARCH}.a" "${OUT_DIR}/builtins"/*.o
echo "    [DONE] ${OUT_DIR}/lib/libclang_rt.builtins-${ARCH}.a"

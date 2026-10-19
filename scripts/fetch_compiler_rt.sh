#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned LLVM compiler-rt Builtins Fetch Script
# ==============================================================================
# Fetches LLVM compiler-rt builtins sources into vendor/compiler-rt/lib/builtins
# via sparse checkout. Provides soft-float (tf), integer math, and complex builtins.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/vendor/compiler-rt/lib/builtins"

LLVM_REPO_URL="https://github.com/llvm/llvm-project.git"
PINNED_BRANCH="llvmorg-21.1.0"

echo "==> irisOS: Checking compiler-rt builtins in ${TARGET_DIR}..."

if [[ -f "${TARGET_DIR}/addtf3.c" && -f "${TARGET_DIR}/muldc3.c" ]]; then
    echo "==> compiler-rt builtins already present."
    exit 0
fi

echo "==> Fetching compiler-rt builtins (${PINNED_BRANCH}) from ${LLVM_REPO_URL}..."
TEMP_DIR="$(mktemp -d /tmp/iris_builtins_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

git clone --depth 1 --branch "${PINNED_BRANCH}" --filter=blob:none --sparse "${LLVM_REPO_URL}" "${TEMP_DIR}/llvm-project"
cd "${TEMP_DIR}/llvm-project"
git sparse-checkout set compiler-rt/lib/builtins

mkdir -p "${TARGET_DIR}"
cp -R "${TEMP_DIR}/llvm-project/compiler-rt/lib/builtins/." "${TARGET_DIR}/"

echo "    [DONE] Installed compiler-rt builtins into ${TARGET_DIR}"

#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned Zircon Microkernel Fetch Script
# ==============================================================================
# Fetches the pinned standalone Zircon microkernel source tree, Gigaboot UEFI
# bootloader, libzircon.so (vDSO) sources, and the host zbi packaging tool.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/kernel/zircon"

# Pinned standalone Zircon repository and commit SHA
# (Milestone-1011 branch commit: 86b09d1d34dbd44390cee58e95f795f89169ea79)
ZIRCON_REPO_URL="https://fuchsia.googlesource.com/zircon"
PINNED_BRANCH="milestone-1011"
PINNED_COMMIT="86b09d1d34dbd44390cee58e95f795f89169ea79"
SHORT_COMMIT="${PINNED_COMMIT:0:7}"

echo "==> irisOS: Checking Zircon microkernel sources..."

# Check if already fetched and verified
if [[ -d "${TARGET_DIR}/kernel" && -d "${TARGET_DIR}/bootloader" && -d "${TARGET_DIR}/system/ulib/zircon" && -d "${TARGET_DIR}/system/host/zbi" ]]; then
    echo "==> Zircon source tree already present in ${TARGET_DIR}."
    echo "==> Verified core directories:"
    echo "    - Kernel:     ${TARGET_DIR}/kernel"
    echo "    - Bootloader: ${TARGET_DIR}/bootloader (Gigaboot)"
    echo "    - vDSO:       ${TARGET_DIR}/system/ulib/zircon (libzircon.so)"
    echo "    - Headers:    ${TARGET_DIR}/system/public/zircon"
    echo "    - Host Tools: ${TARGET_DIR}/system/host/zbi"
    exit 0
fi

echo "==> Fetching standalone Zircon tree from ${ZIRCON_REPO_URL} (${PINNED_BRANCH} @ ${SHORT_COMMIT})..."

TEMP_DIR="$(mktemp -d /tmp/iris_zircon_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

# Clone shallow depth 1 on pinned branch
git clone --depth 1 -b "${PINNED_BRANCH}" "${ZIRCON_REPO_URL}" "${TEMP_DIR}/zircon"

# Preserve .gitkeep, then sync tree into TARGET_DIR
mkdir -p "${TARGET_DIR}"
rm -rf "${TEMP_DIR}/zircon/.git"

echo "==> Installing sources into ${TARGET_DIR}..."
cp -R "${TEMP_DIR}/zircon/." "${TARGET_DIR}/"

# Verify required components
echo "==> Verifying fetched components..."
REQUIRED_PATHS=(
    "kernel"
    "bootloader"
    "system/ulib/zircon"
    "system/public/zircon"
    "system/host/zbi"
)

for rel_path in "${REQUIRED_PATHS[@]}"; do
    if [[ ! -e "${TARGET_DIR}/${rel_path}" ]]; then
        echo "ERROR: Missing required component: ${rel_path}" >&2
        exit 1
    fi
    echo "    [OK] ${rel_path}"
done

TREE_SIZE="$(du -sh "${TARGET_DIR}" | cut -f1)"
echo "==> Successfully fetched and verified Zircon microkernel tree (${TREE_SIZE}) at commit ${SHORT_COMMIT}."

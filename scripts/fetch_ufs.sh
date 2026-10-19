#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned UFS2 / FFS (libufs) Filesystem Fetch Script
# ==============================================================================
# Fetches FreeBSD UFS2/FFS filesystem sources and libufs into vendor/libufs/.
#
# Primary Mirror:    https://github.com/stirbeiu/libufs-standalone.git
# Upstream Fallback: https://github.com/freebsd/freebsd-src.git (releng/14.2)
# License:           BSD 2-Clause (Zone 1 Permissive)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/vendor/libufs"

PRIMARY_URL="https://github.com/stirbeiu/libufs-standalone.git"
FALLBACK_URL="https://github.com/freebsd/freebsd-src.git"
FALLBACK_BRANCH="releng/14.2"

# Support --clean flag to force re-fetch
if [[ "${1:-}" == "--clean" ]]; then
    echo "==> Cleaning existing libufs tree in ${TARGET_DIR}..."
    if [[ -d "${TARGET_DIR}" ]]; then
        find "${TARGET_DIR}" -mindepth 1 -maxdepth 1 ! -name ".gitkeep" -exec rm -rf {} +
    fi
fi

echo "==> irisOS: Checking UFS2 / libufs sources in ${TARGET_DIR}..."

if [[ -d "${TARGET_DIR}/libufs" && -d "${TARGET_DIR}/sys/ufs" && -f "${TARGET_DIR}/LICENSE" ]]; then
    echo "==> UFS2 / libufs source tree already present."
    exit 0
fi

TEMP_DIR="$(mktemp -d /tmp/iris_ufs_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

echo "==> Fetching UFS2 / libufs sources..."

CLONED=0
echo "--> Trying primary mirror: ${PRIMARY_URL}..."
if git clone --depth 1 "${PRIMARY_URL}" "${TEMP_DIR}/mirror" 2>/dev/null; then
    if [[ -d "${TEMP_DIR}/mirror/libufs" && -d "${TEMP_DIR}/mirror/sys/ufs" ]]; then
        echo "    [OK] Successfully cloned UFS2/libufs from primary GitHub mirror."
        rm -rf "${TEMP_DIR}/mirror/.git"
        mkdir -p "${TARGET_DIR}"
        cp -R "${TEMP_DIR}/mirror/." "${TARGET_DIR}/"
        CLONED=1
    else
        echo "    [INFO] Primary mirror is empty or initializing. Falling back to upstream FreeBSD..."
    fi
fi

if [[ "${CLONED}" -ne 1 ]]; then
    echo "    [WARN] Trying upstream fallback: ${FALLBACK_URL} (${FALLBACK_BRANCH})..."
    git clone --depth 1 --filter=blob:none --sparse -b "${FALLBACK_BRANCH}" "${FALLBACK_URL}" "${TEMP_DIR}/freebsd"
    (
        cd "${TEMP_DIR}/freebsd"
        git sparse-checkout set lib/libufs sys/ufs
    )

    mkdir -p "${TARGET_DIR}/libufs" "${TARGET_DIR}/sys"
    cp -R "${TEMP_DIR}/freebsd/lib/libufs/." "${TARGET_DIR}/libufs/"
    cp -R "${TEMP_DIR}/freebsd/sys/ufs" "${TARGET_DIR}/sys/"
    
    # Extract BSD-2-Clause license
    git -C "${TEMP_DIR}/freebsd" show "origin/${FALLBACK_BRANCH}:COPYRIGHT" > "${TARGET_DIR}/LICENSE"

    # Create README for standalone distribution
    cat << 'EOF' > "${TARGET_DIR}/README.md"
# Standalone FreeBSD UFS2 / FFS Filesystem Library (`libufs`)

This repository contains standalone UFS2/FFS filesystem and `libufs` sources extracted
from the FreeBSD project (`releng/14.2`), licensed under the **BSD 2-Clause License**.

## Contents:
- `libufs/`: User-space UFS library for superblock, cylinder group, and inode management (`lib/libufs/`).
- `sys/ufs/`: Core UFS and FFS filesystem definitions and structures (`sys/ufs/ffs/` and `sys/ufs/ufs/`).
- `LICENSE`: BSD 2-Clause copyright notice.
EOF

    echo "    [OK] Successfully fetched and prepared UFS2/libufs from FreeBSD upstream."
    CLONED=1
fi

if [[ "${CLONED}" -ne 1 ]]; then
    echo "ERROR: Failed to fetch UFS2/libufs from both primary mirror and upstream fallback." >&2
    exit 1
fi

echo "==> UFS2 / libufs fetch complete."

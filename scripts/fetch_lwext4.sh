#!/usr/bin/env bash
# ==============================================================================
# irisOS - Fetch lwext4 Submodule / Tree
# ==============================================================================
# Fetches pinned lwext4 source tree (v1.0.0) into vendor/lwext4/.
#
# Primary Mirror:    https://github.com/stirbeiu/lwext4.git
# Upstream Fallback: https://github.com/gkostka/lwext4.git
# License:           BSD 2-Clause (Zone 1 Permissive)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${ROOT_DIR}/vendor"
LWEXT4_DIR="${VENDOR_DIR}/lwext4"

PINNED_TAG="v1.0.0"
PRIMARY_URL="https://github.com/stirbeiu/lwext4.git"
FALLBACK_URL="https://github.com/gkostka/lwext4.git"

# Support --clean flag to force re-fetch
if [[ "${1:-}" == "--clean" ]]; then
    echo "==> Cleaning existing lwext4 tree in ${LWEXT4_DIR}..."
    if [[ -d "${LWEXT4_DIR}" ]]; then
        find "${LWEXT4_DIR}" -mindepth 1 -maxdepth 1 ! -name ".gitkeep" -exec rm -rf {} +
    fi
fi

echo "==> irisOS: Checking lwext4 sources in ${LWEXT4_DIR}..."

if [[ -d "${LWEXT4_DIR}/src" && -f "${LWEXT4_DIR}/CMakeLists.txt" ]]; then
    echo "==> lwext4 source tree already present."
    exit 0
fi

TEMP_DIR="$(mktemp -d /tmp/iris_lwext4_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

echo "==> Fetching lwext4 (${PINNED_TAG})..."

CLONED=0
echo "--> Trying primary mirror: ${PRIMARY_URL}..."
if git clone --depth 1 -b "${PINNED_TAG}" "${PRIMARY_URL}" "${TEMP_DIR}/lwext4" 2>/dev/null; then
    echo "    [OK] Successfully cloned lwext4 from primary GitHub mirror."
    CLONED=1
else
    echo "    [WARN] Primary mirror failed. Trying upstream fallback: ${FALLBACK_URL}..."
    if git clone --depth 1 -b "${PINNED_TAG}" "${FALLBACK_URL}" "${TEMP_DIR}/lwext4"; then
        echo "    [OK] Successfully cloned lwext4 from upstream fallback."
        CLONED=1
    fi
fi

if [[ "${CLONED}" -ne 1 ]]; then
    echo "ERROR: Failed to clone lwext4 from both primary mirror and upstream fallback." >&2
    exit 1
fi

rm -rf "${TEMP_DIR}/lwext4/.git"
mkdir -p "${LWEXT4_DIR}"
cp -R "${TEMP_DIR}/lwext4/." "${LWEXT4_DIR}/"
echo "    [DONE] Installed lwext4 sources into ${LWEXT4_DIR}"
echo "==> lwext4 fetch complete."

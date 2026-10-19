#!/usr/bin/env bash
# ==============================================================================
# irisOS - Fetch lwext4 Submodule / Tree
#
# Canonical Upstream: https://github.com/gkostka/lwext4
# irisOS Mirror:     https://github.com/stirbeiu/lwext4
# License:           BSD 2-Clause (Zone 1 Permissive)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${ROOT_DIR}/vendor"
LWEXT4_DIR="${VENDOR_DIR}/lwext4"

# Pinned upstream commit or tag
PINNED_TAG="v1.0.0"
UPSTREAM_URL="https://github.com/gkostka/lwext4.git"
MIRROR_URL="https://github.com/stirbeiu/lwext4.git"

if [[ -d "${LWEXT4_DIR}/src" ]]; then
    echo "==> vendor/lwext4 is already present."
    exit 0
fi

echo "==> Fetching lwext4 from upstream (${UPSTREAM_URL})..."
mkdir -p "${VENDOR_DIR}"

if git clone --depth 1 --branch "${PINNED_TAG}" "${UPSTREAM_URL}" "${LWEXT4_DIR}" 2>/dev/null; then
    echo "==> Successfully cloned lwext4 from upstream."
else
    echo "==> Upstream clone failed, attempting fallback to mirror (${MIRROR_URL})..."
    git clone --depth 1 "${MIRROR_URL}" "${LWEXT4_DIR}"
fi

echo "==> lwext4 fetch complete."

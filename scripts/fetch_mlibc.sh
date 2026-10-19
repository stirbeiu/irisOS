#!/usr/bin/env bash
# ==============================================================================
# irisOS - Pinned mlibc C Library Fetch Script
# ==============================================================================
# Fetches pinned mlibc source tree (v6.3.1) into vendor/mlibc/ and injects
# the irisOS sysdeps dispatch hook.
#
# Primary Mirror:    https://github.com/stirbeiu/mlibc.git (and frigg.git)
# Upstream Fallback: https://github.com/managarm/mlibc.git (and frigg.git)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/vendor/mlibc"

PRIMARY_MLIBC_URL="https://github.com/stirbeiu/mlibc.git"
FALLBACK_MLIBC_URL="https://github.com/managarm/mlibc.git"
PINNED_TAG="v6.3.1"

PRIMARY_FRIGG_URL="https://github.com/stirbeiu/frigg.git"
FALLBACK_FRIGG_URL="https://github.com/managarm/frigg.git"
FRIGG_COMMIT="98220ab8f3ac6b1f146c360598a334f7f1fc06e1"

# Support --clean flag to force re-fetch
if [[ "${1:-}" == "--clean" ]]; then
    echo "==> Cleaning existing mlibc tree in ${TARGET_DIR}..."
    find "${TARGET_DIR}" -mindepth 1 -maxdepth 1 ! -name ".gitkeep" -exec rm -rf {} +
fi

echo "==> irisOS: Checking mlibc sources in ${TARGET_DIR}..."

if [[ -f "${TARGET_DIR}/meson.build" && -d "${TARGET_DIR}/options" && -d "${TARGET_DIR}/abis" ]]; then
    echo "==> mlibc source tree already present."
else
    echo "==> Fetching mlibc (${PINNED_TAG})..."
    TEMP_DIR="$(mktemp -d /tmp/iris_mlibc_fetch.XXXXXX)"
    cleanup() {
        rm -rf "${TEMP_DIR}"
    }
    trap cleanup EXIT

    CLONED=0
    echo "--> Trying primary mirror: ${PRIMARY_MLIBC_URL}..."
    if git clone --depth 1 -b "${PINNED_TAG}" "${PRIMARY_MLIBC_URL}" "${TEMP_DIR}/mlibc" 2>/dev/null; then
        echo "    [OK] Successfully cloned mlibc from primary GitHub mirror."
        CLONED=1
    else
        echo "    [WARN] Primary mirror failed. Trying upstream fallback: ${FALLBACK_MLIBC_URL}..."
        if git clone --depth 1 -b "${PINNED_TAG}" "${FALLBACK_MLIBC_URL}" "${TEMP_DIR}/mlibc"; then
            echo "    [OK] Successfully cloned mlibc from upstream fallback."
            CLONED=1
        fi
    fi

    if [[ "${CLONED}" -ne 1 ]]; then
        echo "ERROR: Failed to clone mlibc from both primary mirror and upstream fallback." >&2
        exit 1
    fi

    rm -rf "${TEMP_DIR}/mlibc/.git"
    mkdir -p "${TARGET_DIR}"
    cp -R "${TEMP_DIR}/mlibc/." "${TARGET_DIR}/"
    echo "    [DONE] Installed mlibc sources into ${TARGET_DIR}"
fi

# Ensure subprojects/frigg is present
FRIGG_DIR="${TARGET_DIR}/subprojects/frigg"
if [[ ! -d "${FRIGG_DIR}" || ! -f "${FRIGG_DIR}/meson.build" ]]; then
    echo "==> Fetching frigg subproject (${FRIGG_COMMIT})..."
    mkdir -p "${TARGET_DIR}/subprojects"
    rm -rf "${FRIGG_DIR}"

    FRIGG_CLONED=0
    echo "--> Trying primary frigg mirror: ${PRIMARY_FRIGG_URL}..."
    if git clone "${PRIMARY_FRIGG_URL}" "${FRIGG_DIR}" 2>/dev/null; then
        if git -C "${FRIGG_DIR}" checkout "${FRIGG_COMMIT}" 2>/dev/null; then
            echo "    [OK] Successfully fetched frigg from primary mirror."
            FRIGG_CLONED=1
        else
            rm -rf "${FRIGG_DIR}"
        fi
    fi

    if [[ "${FRIGG_CLONED}" -ne 1 ]]; then
        echo "    [WARN] Primary frigg mirror failed. Trying upstream fallback: ${FALLBACK_FRIGG_URL}..."
        git clone "${FALLBACK_FRIGG_URL}" "${FRIGG_DIR}"
        git -C "${FRIGG_DIR}" checkout "${FRIGG_COMMIT}"
        echo "    [OK] Successfully fetched frigg from upstream fallback."
    fi
fi

# Inject irisOS sysdeps hook into vendor/mlibc/meson.build if not already patched
if ! grep -q "host_machine.system() == 'iris'" "${TARGET_DIR}/meson.build"; then
    echo "==> Patching vendor/mlibc/meson.build with irisOS sysdeps dispatch..."
    python3 -c '
with open("'${TARGET_DIR}'/meson.build", "r") as f:
    content = f.read()

target = "if host_machine.system() == '\''linux'\''"
replacement = """if host_machine.system() == '\''iris'\''
	rtld_include_dirs += include_directories('\''sysdeps/iris/include'\'')
	libc_include_dirs += include_directories('\''sysdeps/iris/include'\'')
	internal_conf.set10('\''MLIBC_MAP_DSO_SEGMENTS'\'', true)
	internal_conf.set10('\''MLIBC_MMAP_ALLOCATE_DSO'\'', true)
	subdir('\''sysdeps/iris'\'')
elif host_machine.system() == '\''linux'\''"""

if target in content:
    content = content.replace(target, replacement, 1)
    with open("'${TARGET_DIR}'/meson.build", "w") as f:
        f.write(content)
    print("    [DONE] Successfully injected irisOS dispatch hook.")
else:
    print("    [WARNING] Could not locate insertion point in meson.build.")
'
fi

# Inject symlink from sdk/mlibc-sysdeps/iris into vendor/mlibc/sysdeps/iris
mkdir -p "${TARGET_DIR}/sysdeps"
rm -rf "${TARGET_DIR}/sysdeps/iris"
ln -sf "${REPO_ROOT}/sdk/mlibc-sysdeps/iris" "${TARGET_DIR}/sysdeps/iris"
echo "==> Symlinked sdk/mlibc-sysdeps/iris -> vendor/mlibc/sysdeps/iris"

echo "==> mlibc vendor tree is ready."

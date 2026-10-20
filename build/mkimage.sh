#!/usr/bin/env bash
# ==============================================================================
# irisOS - Disk & UEFI Boot Image Creation Script
#
# Generates:
#  1. Bootable ZBI image (iris_a.zbi) containing kernel, cmdline, and iris_init in BOOTFS
#  2. FAT32 UEFI ESP partition image (esp.img) containing Gigaboot + iris_a.zbi
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${ROOT_DIR}/out"
HOST_TOOLS="${OUT_DIR}/host"
ZIRCON_DIR="${ROOT_DIR}/kernel/zircon"

ARCH="${1:-x86_64}"

case "${ARCH}" in
    x86_64|x64)
        ARCH_NAME="x86_64"
        ZBI_ARCH="x64"
        PROJECT_NAME="x64"
        EFI_BIN_NAME="BOOTX64.EFI"
        ZIRCON_BUILD="${ZIRCON_DIR}/build-x64-clang"
        BUILD_DIR="${OUT_DIR}/build-x86_64"
        CMDLINE_ARGS="userboot=bin/iris_init zircon.nodename=irisOS-dev kernel.serial=legacy gfxconsole.early=true"
        ;;
    aarch64|arm64)
        ARCH_NAME="arm64"
        ZBI_ARCH="arm64"
        PROJECT_NAME="arm64"
        EFI_BIN_NAME="BOOTAA64.EFI"
        ZIRCON_BUILD="${ZIRCON_DIR}/build-arm64-clang"
        BUILD_DIR="${OUT_DIR}/build-arm64"
        CMDLINE_ARGS="userboot=bin/iris_init zircon.nodename=irisOS-dev kernel.serial=legacy gfxconsole.early=true"
        ;;
    *)
        echo "Error: Unsupported architecture '${ARCH}'. Use 'x86_64' or 'arm64'."
        exit 1
        ;;
esac

echo "==> [mkimage] Packaging irisOS boot image for ${ARCH_NAME}..."

# 1. Ensure host tools (zbi) exist
if [[ ! -x "${HOST_TOOLS}/zbi" ]]; then
    echo "==> Building host zbi utility..."
    make -C "${ROOT_DIR}/build" zbi
fi

# 2. Ensure iris_init, its_irisOS, virtio_blk, and iris_crypt binaries exist
if [[ ! -f "${BUILD_DIR}/bin/iris_init" ]]; then
    echo "==> Building iris_init for ${ARCH_NAME}..."
    make -C "${ROOT_DIR}/build" "build-${ARCH_NAME}"
fi
if [[ ! -f "${BUILD_DIR}/bin/its_irisOS" ]]; then
    echo "==> Building its_irisOS for ${ARCH_NAME}..."
    make -C "${ROOT_DIR}/build" "its-irisOS-${ARCH_NAME}"
fi
if [[ ! -f "${BUILD_DIR}/bin/virtio_blk" ]]; then
    echo "==> Building virtio_blk for ${ARCH_NAME}..."
    make -C "${ROOT_DIR}/build" "virtio-blk-${ARCH_NAME}"
fi
if [[ ! -f "${BUILD_DIR}/bin/iris_crypt" ]]; then
    echo "==> Building iris_crypt for ${ARCH_NAME}..."
    make -C "${ROOT_DIR}/build" "iris-crypt-${ARCH_NAME}"
fi

# 3. Ensure Zircon kernel ZBI exists
if [[ ! -f "${ZIRCON_BUILD}/kernel.zbi" ]]; then
    echo "==> Building Zircon kernel for ${ARCH_NAME}..."
    make -C "${ROOT_DIR}/build" "kernel-${ARCH_NAME}"
fi

# 4. Ensure Gigaboot EFI binary exists
EFI_SRC="${ZIRCON_BUILD}/bootloader/$(echo "${EFI_BIN_NAME}" | tr '[:upper:]' '[:lower:]')"
if [[ ! -f "${EFI_SRC}" ]]; then
    echo "==> Compiling Gigaboot UEFI bootloader (${EFI_SRC})..."
    make -C "${ZIRCON_DIR}" \
        "build-${ZBI_ARCH}-clang/bootloader/$(echo "${EFI_BIN_NAME}" | tr '[:upper:]' '[:lower:]')" \
        PROJECT="${PROJECT_NAME}" \
        USE_CLANG=true \
        USE_LLD=true \
        CLANG_TOOLCHAIN_PREFIX="/opt/homebrew/opt/llvm/bin/" \
        "ARCH_${ARCH_NAME}_TOOLCHAIN_INCLUDED=1" \
        ENABLE_BUILD_SYSROOT=false
fi

# 5. Package iris_a.zbi
mkdir -p "${BUILD_DIR}"
IRIS_ZBI="${BUILD_DIR}/iris_a.zbi"
echo "==> Creating bootable ZBI: ${IRIS_ZBI}"

EXTRA_ZBI_INPUTS=()
if [[ "${ARCH_NAME}" == "arm64" ]]; then
    QEMU_BOARD_ZBI="${BUILD_DIR}/qemu_board.zbi"
    python3 "${ROOT_DIR}/scripts/gen_qemu_arm64_zbi.py" "${QEMU_BOARD_ZBI}"
    EXTRA_ZBI_INPUTS+=("${QEMU_BOARD_ZBI}")
fi

"${HOST_TOOLS}/zbi" -o "${IRIS_ZBI}" \
    --complete="${ZBI_ARCH}" \
    "${ZIRCON_BUILD}/kernel.zbi" \
    ${EXTRA_ZBI_INPUTS[@]+"${EXTRA_ZBI_INPUTS[@]}"} \
    --type CMDLINE -e "${CMDLINE_ARGS}" \
    --files -e "bin/iris_init=${BUILD_DIR}/bin/iris_init" \
            -e "bin/its_irisOS=${BUILD_DIR}/bin/its_irisOS" \
            -e "bin/virtio_blk=${BUILD_DIR}/bin/virtio_blk" \
            -e "bin/iris_crypt=${BUILD_DIR}/bin/iris_crypt"

# 6. Construct FAT32 UEFI ESP disk image
IMAGES_DIR="${OUT_DIR}/images"
mkdir -p "${IMAGES_DIR}"
ESP_IMG="${IMAGES_DIR}/esp_${ARCH_NAME}.img"
echo "==> Constructing FAT32 ESP image (${ESP_IMG}, 64MB)..."

rm -f "${ESP_IMG}"
dd if=/dev/zero of="${ESP_IMG}" bs=1M count=64 status=none
/opt/homebrew/bin/mformat -i "${ESP_IMG}" -F -v "ESP" ::
/opt/homebrew/bin/mmd -i "${ESP_IMG}" ::EFI
/opt/homebrew/bin/mmd -i "${ESP_IMG}" ::EFI/BOOT
/opt/homebrew/bin/mcopy -i "${ESP_IMG}" "${EFI_SRC}" "::EFI/BOOT/${EFI_BIN_NAME}"
/opt/homebrew/bin/mcopy -i "${ESP_IMG}" "${IRIS_ZBI}" "::EFI/BOOT/iris_a.zbi"

# Symlink esp.img to current architecture
ln -sf "esp_${ARCH_NAME}.img" "${IMAGES_DIR}/esp.img"

# 7. Construct 64MB raw virtual disk image for virtio-blk-pci testing
DISK_RAW="${IMAGES_DIR}/disk_${ARCH_NAME}.raw"
if [[ ! -f "${DISK_RAW}" ]]; then
    echo "==> Creating 64MB raw virtual disk image (${DISK_RAW})..."
    dd if=/dev/zero of="${DISK_RAW}" bs=1M count=64 status=none
fi

echo "==> [mkimage] Success! Created ${ESP_IMG} and ${DISK_RAW}"

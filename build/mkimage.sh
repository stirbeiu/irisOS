#!/usr/bin/env bash
# ==============================================================================
# irisOS - 4-Partition GPT Disk & UEFI Boot Image Creation Script
#
# Generates:
#  1. Dual Bootable ZBIs (iris_a.zbi, iris_b.zbi) containing Zircon kernel,
#     cmdline, and BOOTFS (iris_init, virtio_blk, iris_crypt, iris_fshost, its_irisOS)
#  2. Partition 1 (ESP):      32 MB FAT32 (Gigaboot + iris_a.zbi + iris_b.zbi)
#  3. Partition 2 (System_A): 32 MB FreeBSD UFS2 (active immutable system root)
#  4. Partition 3 (System_B): 32 MB FreeBSD UFS2 (inactive A/B rollback slot)
#  5. Partition 4 (Data):     30 MB Encrypted UFS2 (AES-256-XTS + 4 KB Sector 0 header)
#  6. 128 MB GPT Raw Disk Image (disk_<arch>.raw / disk.img) with Protective MBR
#     and Primary + Backup GPT headers
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${ROOT_DIR}/out"
HOST_TOOLS="${OUT_DIR}/host"
ZIRCON_DIR="${ROOT_DIR}/kernel/zircon"

ARCH="${1:-x86_64}"

case "${ARCH}" in
    x86_64)
        ARCH_NAME="x86_64"
        ZBI_ARCH="x64"
        PROJECT_NAME="x64"
        TOOLCHAIN_ARCH="x86_64"
        EFI_BIN_NAME="BOOTX64.EFI"
        ZIRCON_BUILD="${ZIRCON_DIR}/build-x64-clang"
        BUILD_DIR="${OUT_DIR}/build-x86_64"
        CMDLINE_ARGS="userboot=bin/iris_init zircon.nodename=irisOS-dev kernel.serial=legacy gfxconsole.early=true"
        ;;
    aarch64)
        ARCH_NAME="aarch64"
        ZBI_ARCH="arm64"
        PROJECT_NAME="arm64"
        TOOLCHAIN_ARCH="arm64"
        EFI_BIN_NAME="BOOTAA64.EFI"
        ZIRCON_BUILD="${ZIRCON_DIR}/build-arm64-clang"
        BUILD_DIR="${OUT_DIR}/build-aarch64"
        CMDLINE_ARGS="userboot=bin/iris_init zircon.nodename=irisOS-dev kernel.serial=legacy gfxconsole.early=true"
        ;;
    *)
        echo "Error: Unsupported architecture '${ARCH}'. Use 'x86_64' or 'aarch64'."
        exit 1
        ;;
esac

echo "==> [mkimage] Packaging irisOS 4-partition GPT disk image for ${ARCH_NAME}..."

# 1. Ensure host tools (zbi, mkufs2) exist
if [[ ! -x "${HOST_TOOLS}/zbi" ]]; then
    echo "==> Building host zbi utility..."
    make -C "${ROOT_DIR}/build" zbi
fi
if [[ ! -x "${HOST_TOOLS}/mkufs2" ]]; then
    echo "==> Building host mkufs2 formatter..."
    make -C "${ROOT_DIR}/build" mkufs2
fi

# 2. Ensure iris_init, its_irisOS, virtio_blk, iris_crypt, iris_fshost, and iris_pty binaries exist
if [[ ! -f "${BUILD_DIR}/bin/iris_init" || ! -f "${BUILD_DIR}/bin/its_irisOS" || ! -f "${BUILD_DIR}/bin/virtio_blk" || ! -f "${BUILD_DIR}/bin/iris_crypt" || ! -f "${BUILD_DIR}/bin/iris_fshost" || ! -f "${BUILD_DIR}/bin/iris_pty" ]]; then
    echo "==> Building server binaries for ${ARCH_NAME}..."
    make -C "${ROOT_DIR}/build" "build-${ARCH_NAME}"
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
        "ARCH_${TOOLCHAIN_ARCH}_TOOLCHAIN_INCLUDED=1" \
        ENABLE_BUILD_SYSROOT=false
fi

# 5. Package Dual ZBIs (iris_a.zbi and iris_b.zbi)
mkdir -p "${BUILD_DIR}"
IRIS_ZBI_A="${BUILD_DIR}/iris_a.zbi"
IRIS_ZBI_B="${BUILD_DIR}/iris_b.zbi"
echo "==> Creating bootable ZBIs: ${IRIS_ZBI_A} & ${IRIS_ZBI_B}"

EXTRA_ZBI_INPUTS=()
if [[ "${ARCH_NAME}" == "aarch64" ]]; then
    QEMU_BOARD_ZBI="${BUILD_DIR}/qemu_board.zbi"
    python3 "${ROOT_DIR}/scripts/gen_qemu_aarch64_zbi.py" "${QEMU_BOARD_ZBI}"
    EXTRA_ZBI_INPUTS+=("${QEMU_BOARD_ZBI}")
fi

"${HOST_TOOLS}/zbi" -o "${IRIS_ZBI_A}" \
    --complete="${ZBI_ARCH}" \
    "${ZIRCON_BUILD}/kernel.zbi" \
    ${EXTRA_ZBI_INPUTS[@]+"${EXTRA_ZBI_INPUTS[@]}"} \
    --type CMDLINE -e "${CMDLINE_ARGS}" \
    --files -e "bin/iris_init=${BUILD_DIR}/bin/iris_init" \
            -e "bin/its_irisOS=${BUILD_DIR}/bin/its_irisOS" \
            -e "bin/virtio_blk=${BUILD_DIR}/bin/virtio_blk" \
            -e "bin/iris_crypt=${BUILD_DIR}/bin/iris_crypt" \
            -e "bin/iris_fshost=${BUILD_DIR}/bin/iris_fshost" \
            -e "bin/iris_pty=${BUILD_DIR}/bin/iris_pty"

cp -f "${IRIS_ZBI_A}" "${IRIS_ZBI_B}"

# 6. Construct NVMe Boot ESP (64 MB FAT32) & GPT Partition 1 ESP (32 MB FAT)
IMAGES_DIR="${OUT_DIR}/images"
mkdir -p "${IMAGES_DIR}"
ESP_IMG="${IMAGES_DIR}/esp_${ARCH_NAME}.img"
echo "==> Formatting NVMe ESP image (${ESP_IMG}, 64 MB FAT32)..."

rm -f "${ESP_IMG}"
dd if=/dev/zero of="${ESP_IMG}" bs=1M count=64 status=none
/opt/homebrew/bin/mformat -i "${ESP_IMG}" -F -v "ESP" ::
/opt/homebrew/bin/mmd -i "${ESP_IMG}" ::EFI
/opt/homebrew/bin/mmd -i "${ESP_IMG}" ::EFI/BOOT
/opt/homebrew/bin/mcopy -i "${ESP_IMG}" "${EFI_SRC}" "::EFI/BOOT/${EFI_BIN_NAME}"
/opt/homebrew/bin/mcopy -i "${ESP_IMG}" "${IRIS_ZBI_A}" "::EFI/BOOT/iris_a.zbi"
/opt/homebrew/bin/mcopy -i "${ESP_IMG}" "${IRIS_ZBI_B}" "::EFI/BOOT/iris_b.zbi"
ln -sf "esp_${ARCH_NAME}.img" "${IMAGES_DIR}/esp.img"

PART1_ESP="${IMAGES_DIR}/part1_esp_${ARCH_NAME}.img"
echo "==> Formatting GPT Partition 1 (ESP): 32 MB FAT..."
rm -f "${PART1_ESP}"
dd if=/dev/zero of="${PART1_ESP}" bs=512 count=65536 status=none
/opt/homebrew/bin/mformat -i "${PART1_ESP}" -v "ESP" ::
/opt/homebrew/bin/mmd -i "${PART1_ESP}" ::EFI
/opt/homebrew/bin/mmd -i "${PART1_ESP}" ::EFI/BOOT
/opt/homebrew/bin/mcopy -i "${PART1_ESP}" "${EFI_SRC}" "::EFI/BOOT/${EFI_BIN_NAME}"
/opt/homebrew/bin/mcopy -i "${PART1_ESP}" "${IRIS_ZBI_A}" "::EFI/BOOT/iris_a.zbi"
/opt/homebrew/bin/mcopy -i "${PART1_ESP}" "${IRIS_ZBI_B}" "::EFI/BOOT/iris_b.zbi"

# 7. Construct Partition 2 (System_A) & Partition 3 (System_B): 32 MB UFS2 each (65536 sectors)
SYS_A_IMG="${IMAGES_DIR}/system_a_${ARCH_NAME}.ufs2"
SYS_B_IMG="${IMAGES_DIR}/system_b_${ARCH_NAME}.ufs2"
echo "==> Formatting Partition 2 (System_A) & Partition 3 (System_B): 32 MB UFS2..."
"${HOST_TOOLS}/mkufs2" "${SYS_A_IMG}" 65536 system System_A
"${HOST_TOOLS}/mkufs2" "${SYS_B_IMG}" 65536 system System_B

# 8. Construct Partition 4 (Data): 30 MB AES-256-XTS Encrypted UFS2 (61440 sectors)
DATA_IMG="${IMAGES_DIR}/data_${ARCH_NAME}.crypt"
echo "==> Formatting Partition 4 (Data): 30 MB AES-256-XTS Encrypted UFS2..."
"${HOST_TOOLS}/mkufs2" "${DATA_IMG}" 61440 data-encrypted Data

# 9. Assemble 128 MB 4-Partition GPT Raw Disk Image (262,144 sectors)
DISK_RAW="${IMAGES_DIR}/disk_${ARCH_NAME}.raw"
echo "==> Assembling 128 MB 4-Partition GPT disk image (${DISK_RAW})..."
python3 "${ROOT_DIR}/scripts/mk_gpt_disk.py" \
    "${DISK_RAW}" \
    "${PART1_ESP}" \
    "${SYS_A_IMG}" \
    "${SYS_B_IMG}" \
    "${DATA_IMG}"

ln -sf "disk_${ARCH_NAME}.raw" "${IMAGES_DIR}/disk.img"
rm -f "${PART1_ESP}" "${SYS_A_IMG}" "${SYS_B_IMG}" "${DATA_IMG}"

echo "==> [mkimage] Success! Created 128 MB GPT disk image: ${DISK_RAW} (and ${IMAGES_DIR}/disk.img)"

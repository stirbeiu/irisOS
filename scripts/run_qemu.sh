#!/usr/bin/env bash
# ==============================================================================
# irisOS - QEMU UEFI Virtual Machine Launcher
#
# Supports:
#  - x86_64  (q35 machine with EDK2 OVMF UEFI, NVMe ESP boot, and VirtIO disk)
#  - aarch64 (virt machine with EDK2 AAVMF UEFI, NVMe ESP boot, and VirtIO disk)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGES_DIR="${ROOT_DIR}/out/images"

ARCH="x86_64"
GUI="${GUI:-1}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -a|--arch)
            ARCH="$2"
            shift 2
            ;;
        -g|--gui)
            GUI=1
            shift
            ;;
        --headless)
            GUI=0
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [-a x86_64|aarch64] [--headless]"
            echo "Defaults to opening a QEMU window with serial console output in stdio."
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [-a x86_64|aarch64] [--headless]"
            exit 1
            ;;
    esac
done

case "${ARCH}" in
    x86_64)
        ARCH_NAME="x86_64"
        QEMU_BIN="qemu-system-x86_64"
        UEFI_CODE="/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
        ESP_IMAGE="${IMAGES_DIR}/esp_x86_64.img"
        ;;
    aarch64)
        ARCH_NAME="aarch64"
        QEMU_BIN="qemu-system-aarch64"
        UEFI_CODE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
        UEFI_VARS_SRC="/opt/homebrew/share/qemu/edk2-arm-vars.fd"
        UEFI_VARS_TMP="/tmp/edk2-aarch64-vars-$$.fd"
        trap 'rm -f "${UEFI_VARS_TMP}" 2>/dev/null || true' EXIT
        ESP_IMAGE="${IMAGES_DIR}/esp_aarch64.img"
        ;;
    *)
        echo "Error: Unsupported architecture '${ARCH}'. Use 'x86_64' or 'aarch64'."
        exit 1
        ;;
esac

# The boot mode lives in the ZBI kernel command line, so repackage the images in
# interactive "shell" mode on every launch (./iris test packages "test" mode).
echo "==> Packaging ${ARCH_NAME} image in shell mode (build/mkimage.sh)..."
IRIS_MODE=shell "${ROOT_DIR}/build/mkimage.sh" "${ARCH_NAME}" > /dev/null

# Ensure secondary virtual raw disk exists
DISK_RAW="${IMAGES_DIR}/disk_${ARCH_NAME}.raw"
if [[ ! -f "${DISK_RAW}" ]]; then
    echo "==> Virtual raw disk not found. Creating 64MB disk image (${DISK_RAW})..."
    dd if=/dev/zero of="${DISK_RAW}" bs=1M count=64 status=none
fi

# Ensure UEFI firmware exists
if [[ ! -f "${UEFI_CODE}" ]]; then
    echo "Error: UEFI firmware file not found at ${UEFI_CODE}."
    echo "Please ensure Homebrew QEMU is installed: brew install qemu"
    exit 1
fi

if [[ "${GUI}" == "1" ]]; then
    DISPLAY_MODE="QEMU Cocoa Window (-display cocoa) + Serial Console (-serial stdio)"
    DISPLAY_ARGS=("-display" "cocoa")
else
    DISPLAY_MODE="Headless Serial Console Only (-display none)"
    DISPLAY_ARGS=("-display" "none")
fi

echo "======================================================================"
echo " Launching irisOS (${ARCH_NAME}) in QEMU with UEFI..."
echo " Bootloader: Gigaboot (UEFI)"
echo " Image:      ${ESP_IMAGE}"
echo " Disk:       ${DISK_RAW}"
echo " Firmware:   ${UEFI_CODE}"
echo " Display:    ${DISPLAY_MODE}"
echo " (Press Ctrl+A then X in terminal to terminate QEMU, or close the GUI window)"
echo "======================================================================"

# Note: ESP_IMAGE is attached via NVMe (bootindex=0) so that DISK_RAW (hd0) is
# the sole VirtIO Block device (0x1AF4:0x1001) on the PCIe bus. This prevents
# Ring 3 block tests/encryption on hd0 from ever touching the FAT32 UEFI ESP.
if [[ "${ARCH_NAME}" == "x86_64" ]]; then
    exec "${QEMU_BIN}" \
        -machine q35 \
        -cpu max \
        -m 512M \
        -smp 2 \
        -nic none \
        -drive if=pflash,format=raw,readonly=on,file="${UEFI_CODE}" \
        -drive id=bootdisk,file="${ESP_IMAGE}",format=raw,if=none \
        -device nvme,drive=bootdisk,serial=irisboot,bootindex=0 \
        -drive file="${DISK_RAW}",if=none,id=hd0,format=raw \
        -device virtio-blk-pci,drive=hd0 \
        -device virtio-keyboard-pci \
        -device virtio-rng-pci \
        -serial stdio \
        "${DISPLAY_ARGS[@]}"
else
    # Prepare writable vars for aarch64 UEFI
    cp -f "${UEFI_VARS_SRC}" "${UEFI_VARS_TMP}"

    AARCH64_EXTRA_ARGS=()
    if [[ "${GUI}" == "1" ]]; then
        AARCH64_EXTRA_ARGS+=("-device" "ramfb")
    fi

    exec "${QEMU_BIN}" \
        -M virt,gic-version=2 \
        -cpu max \
        -m 512M \
        -smp 2 \
        -nic none \
        -drive if=pflash,format=raw,readonly=on,file="${UEFI_CODE}" \
        -drive if=pflash,format=raw,file="${UEFI_VARS_TMP}" \
        -drive id=bootdisk,file="${ESP_IMAGE}",format=raw,if=none \
        -device nvme,drive=bootdisk,serial=irisboot,bootindex=0 \
        -drive file="${DISK_RAW}",if=none,id=hd0,format=raw \
        -device virtio-blk-pci,drive=hd0 \
        -device virtio-keyboard-pci \
        -device virtio-rng-pci \
        ${AARCH64_EXTRA_ARGS[@]+"${AARCH64_EXTRA_ARGS[@]}"} \
        -serial stdio \
        "${DISPLAY_ARGS[@]}"
fi

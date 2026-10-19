#!/usr/bin/env bash
# ==============================================================================
# irisOS - QEMU UEFI Virtual Machine Launcher
#
# Supports:
#  - x86_64 (q35 machine with EDK2 OVMF UEFI and VirtIO disk)
#  - arm64  (virt machine with EDK2 AAVMF UEFI and VirtIO disk)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGES_DIR="${ROOT_DIR}/out/images"

ARCH="x86_64"
GUI="${GUI:-0}"

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
        -h|--help)
            echo "Usage: $0 [-a x86_64|arm64] [-g|--gui]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [-a x86_64|arm64] [-g|--gui]"
            exit 1
            ;;
    esac
done

case "${ARCH}" in
    x86_64|x64)
        ARCH_NAME="x86_64"
        QEMU_BIN="qemu-system-x86_64"
        UEFI_CODE="/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
        ESP_IMAGE="${IMAGES_DIR}/esp_x86_64.img"
        ;;
    aarch64|arm64)
        ARCH_NAME="arm64"
        QEMU_BIN="qemu-system-aarch64"
        UEFI_CODE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
        UEFI_VARS_SRC="/opt/homebrew/share/qemu/edk2-arm-vars.fd"
        UEFI_VARS_TMP="/tmp/edk2-arm-vars.fd"
        ESP_IMAGE="${IMAGES_DIR}/esp_arm64.img"
        ;;
    *)
        echo "Error: Unsupported architecture '${ARCH}'. Use 'x86_64' or 'arm64'."
        exit 1
        ;;
esac

# Ensure ESP disk image exists
if [[ ! -f "${ESP_IMAGE}" ]]; then
    echo "==> ESP image not found. Building with build/mkimage.sh ${ARCH_NAME}..."
    "${ROOT_DIR}/build/mkimage.sh" "${ARCH_NAME}"
fi

# Ensure UEFI firmware exists
if [[ ! -f "${UEFI_CODE}" ]]; then
    echo "Error: UEFI firmware file not found at ${UEFI_CODE}."
    echo "Please ensure Homebrew QEMU is installed: brew install qemu"
    exit 1
fi

if [[ "${GUI}" == "1" ]]; then
    DISPLAY_MODE="Cocoa GUI Window (-display cocoa)"
    DISPLAY_ARGS=("-display" "cocoa")
else
    DISPLAY_MODE="Headless Serial Console Only (-display none)"
    DISPLAY_ARGS=("-display" "none")
fi

echo "======================================================================"
echo " Launching irisOS (${ARCH_NAME}) in QEMU with UEFI..."
echo " Bootloader: Gigaboot (UEFI)"
echo " Image:      ${ESP_IMAGE}"
echo " Firmware:   ${UEFI_CODE}"
echo " Display:    ${DISPLAY_MODE}"
echo " (Press Ctrl+A then X in terminal to terminate QEMU, or close the GUI window)"
echo "======================================================================"

if [[ "${ARCH_NAME}" == "x86_64" ]]; then
    exec "${QEMU_BIN}" \
        -machine q35 \
        -m 512M \
        -smp 2 \
        -drive if=pflash,format=raw,readonly=on,file="${UEFI_CODE}" \
        -drive file="${ESP_IMAGE}",format=raw,if=virtio \
        -serial stdio \
        "${DISPLAY_ARGS[@]}"
else
    # Prepare writable vars for arm64 UEFI
    cp -f "${UEFI_VARS_SRC}" "${UEFI_VARS_TMP}"

    ARM_EXTRA_ARGS=()
    if [[ "${GUI}" == "1" ]]; then
        ARM_EXTRA_ARGS+=("-device" "ramfb")
    fi

    exec "${QEMU_BIN}" \
        -M virt,gic-version=2 \
        -cpu cortex-a53 \
        -m 512M \
        -smp 2 \
        -drive if=pflash,format=raw,readonly=on,file="${UEFI_CODE}" \
        -drive if=pflash,format=raw,file="${UEFI_VARS_TMP}" \
        -device virtio-blk-pci,drive=bootdisk \
        -drive id=bootdisk,file="${ESP_IMAGE}",format=raw,if=none \
        ${ARM_EXTRA_ARGS[@]+"${ARM_EXTRA_ARGS[@]}"} \
        -serial stdio \
        "${DISPLAY_ARGS[@]}"
fi

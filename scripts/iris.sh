#!/usr/bin/env bash
# ==============================================================================
# irisOS - Master CLI Build & Run Orchestrator
# ==============================================================================
# Provides a unified, ergonomic interface for:
#   - Building (all, x86_64, aarch64)
#   - Running in QEMU with graphical window + serial console (all, x86_64, aarch64)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
IMAGES_DIR="${ROOT_DIR}/out/images"

show_help() {
    cat << 'EOF'
======================================================================
  irisOS - Build & Run Management CLI
======================================================================

USAGE:
  ./iris build <arch>    Build system images
  ./iris run <arch>      Launch VM in QEMU window with serial console
  ./iris clean           Clean all build artifacts

TARGET ARCHITECTURES:
  all                    Both x86_64 and aarch64 architectures
  x86_64                 64-bit x86 architecture (UEFI q35)
  aarch64 (or arm64)     64-bit ARM architecture (UEFI virt)

BUILD COMMANDS:
  make build all         Build system images for both x86_64 and aarch64
  make build x86_64      Build system image for x86_64
  make build aarch64     Build system image for aarch64

RUN COMMANDS (QEMU window + serial console in stdio):
  make run all           Boot dual VMs (x86_64 & aarch64) with warning prompt
  make run x86_64        Boot x86_64 VM (without rebuilding)
  make run aarch64       Boot aarch64 VM (without rebuilding)

(You can also use './iris build ...' and './iris run ...' directly)
======================================================================
EOF
}

cmd_build() {
    local target="${1:-}"
    case "${target}" in
        all)
            echo "======================================================================"
            echo " [irisOS] Building for ALL architectures (x86_64 & aarch64)..."
            echo "======================================================================"
            echo "==> (1/2) Building x86_64..."
            make -C "${BUILD_DIR}" image-x86_64
            echo "==> (2/2) Building aarch64..."
            make -C "${BUILD_DIR}" image-arm64
            echo "======================================================================"
            echo " [irisOS] Successfully built images for both architectures!"
            echo "   - x86_64:  ${IMAGES_DIR}/esp_x86_64.img"
            echo "   - aarch64: ${IMAGES_DIR}/esp_arm64.img"
            echo "======================================================================"
            ;;
        x86_64|x64)
            echo "======================================================================"
            echo " [irisOS] Building for x86_64..."
            echo "======================================================================"
            make -C "${BUILD_DIR}" image-x86_64
            echo "======================================================================"
            echo " [irisOS] x86_64 build complete: ${IMAGES_DIR}/esp_x86_64.img"
            echo "======================================================================"
            ;;
        aarch64|arm64)
            echo "======================================================================"
            echo " [irisOS] Building for aarch64..."
            echo "======================================================================"
            make -C "${BUILD_DIR}" image-arm64
            echo "======================================================================"
            echo " [irisOS] aarch64 build complete: ${IMAGES_DIR}/esp_arm64.img"
            echo "======================================================================"
            ;;
        "")
            echo "Error: Missing architecture target for 'build'."
            echo "Usage: ./iris build [all | x86_64 | aarch64]"
            exit 1
            ;;
        *)
            echo "Error: Unknown build target '${target}'."
            echo "Usage: ./iris build [all | x86_64 | aarch64]"
            exit 1
            ;;
    esac
}

cmd_run() {
    local target="${1:-}"
    case "${target}" in
        all)
            cat << 'EOF'
======================================================================
  WARNING: Dual-Architecture VM Launch
======================================================================
You requested to boot both architectures simultaneously:
  - x86_64  (UEFI q35 machine)
  - aarch64 (UEFI ARM virt machine)

This will launch 2 concurrent QEMU instances in graphical windows
and multiplex their serial consoles directly to this terminal.
======================================================================
EOF
            local response=""
            read -r -p "Do you want to continue? [y/N]: " response || response="n"

            if [[ ! "${response}" =~ ^[yY]([eE][sS])?$ ]]; then
                echo "==> Operation cancelled by user."
                exit 0
            fi

            # Ensure both images exist before launching
            if [[ ! -f "${IMAGES_DIR}/esp_x86_64.img" ]]; then
                echo "==> x86_64 image not found. Building first..."
                make -C "${BUILD_DIR}" image-x86_64
            fi
            if [[ ! -f "${IMAGES_DIR}/esp_arm64.img" ]]; then
                echo "==> aarch64 image not found. Building first..."
                make -C "${BUILD_DIR}" image-arm64
            fi

            echo "==> Launching dual VMs (Ctrl+C to terminate both)..."
            "${SCRIPT_DIR}/run_qemu.sh" -a x86_64 2>&1 | awk '{ print "[x86_64]  " $0; fflush(); }' &
            PID_X86=$!

            "${SCRIPT_DIR}/run_qemu.sh" -a arm64 2>&1 | awk '{ print "[aarch64] " $0; fflush(); }' &
            PID_ARM=$!

            cleanup() {
                echo ""
                echo "==> Shutting down dual QEMU instances (${PID_X86}, ${PID_ARM})..."
                kill -TERM "${PID_X86}" "${PID_ARM}" 2>/dev/null || true
                wait "${PID_X86}" "${PID_ARM}" 2>/dev/null || true
            }
            trap cleanup INT TERM EXIT

            wait "${PID_X86}" "${PID_ARM}" 2>/dev/null || true
            ;;
        x86_64|x64)
            # Run existing image in QEMU window with serial console
            exec "${SCRIPT_DIR}/run_qemu.sh" -a x86_64
            ;;
        aarch64|arm64)
            # Run existing image in QEMU window with serial console
            exec "${SCRIPT_DIR}/run_qemu.sh" -a arm64
            ;;
        "")
            echo "Error: Missing architecture target for 'run'."
            echo "Usage: ./iris run [all | x86_64 | aarch64]"
            exit 1
            ;;
        *)
            echo "Error: Unknown run target '${target}'."
            echo "Usage: ./iris run [all | x86_64 | aarch64]"
            exit 1
            ;;
    esac
}

cmd_clean() {
    make -C "${BUILD_DIR}" clean
}

# Entrypoint dispatcher
COMMAND="${1:-help}"
shift || true

case "${COMMAND}" in
    build)
        cmd_build "$@"
        ;;
    run)
        cmd_run "$@"
        ;;
    clean)
        cmd_clean "$@"
        ;;
    help|-h|--help)
        show_help
        ;;
    *)
        echo "Error: Unknown command '${COMMAND}'."
        show_help
        exit 1
        ;;
esac

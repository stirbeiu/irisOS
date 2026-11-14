#!/usr/bin/env bash
# ==============================================================================
# irisOS - Master CLI Build, Run & Developer Toolchain Orchestrator
# ==============================================================================
# Commands:
#   ./iris build      <all|x86_64|aarch64>   Build images & refresh repo graph
#   ./iris run        <all|x86_64|aarch64>   Launch VM in QEMU + serial console
#   ./iris reset-data <all|x86_64|aarch64>   Wipe & re-provision Data partition
#   ./iris status                            Show built image & disk status
#   ./iris doctor                            Check host toolchain & assets
#   ./iris graph      [query]                Regenerate or query repo_graph
#   ./iris clean                             Clean build artifacts
# ==============================================================================

set -euo pipefail

SOURCE_PATH="${BASH_SOURCE[0]}"
while [[ -L "${SOURCE_PATH}" ]]; do
    SOURCE_DIR="$(cd -P "$(dirname "${SOURCE_PATH}")" && pwd)"
    SOURCE_PATH="$(readlink "${SOURCE_PATH}")"
    [[ "${SOURCE_PATH}" != /* ]] && SOURCE_PATH="${SOURCE_DIR}/${SOURCE_PATH}"
done
SCRIPT_DIR="$(cd -P "$(dirname "${SOURCE_PATH}")" && pwd)"
ROOT_DIR="$(cd -P "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
IMAGES_DIR="${ROOT_DIR}/out/images"

# TTY-aware color helpers
if [[ -t 1 ]]; then
    C_RESET=$'\033[0m'
    C_BOLD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_CYAN=$'\033[36m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_RED=$'\033[31m'
else
    C_RESET="" C_BOLD="" C_DIM="" C_CYAN="" C_GREEN="" C_YELLOW="" C_RED=""
fi

log_info()  { echo "${C_CYAN}==>${C_RESET} $*"; }
log_ok()    { echo "${C_GREEN}[OK]${C_RESET} $*"; }
log_warn()  { echo "${C_YELLOW}[WARN]${C_RESET} $*"; }
log_err()   { echo "${C_RED}[ERR]${C_RESET} $*" >&2; }

banner() {
    echo "${C_BOLD}======================================================================${C_RESET}"
    echo "  $*"
    echo "${C_BOLD}======================================================================${C_RESET}"
}

show_help() {
    cat << 'EOF'
======================================================================
  irisOS - Build, Run & Developer Management CLI
======================================================================

USAGE:
  ./iris build <arch>        Build kernel, userland & GPT disk images
  ./iris run <arch> [args]   Launch QEMU graphical VM + serial console
  ./iris reset-data <arch>   Wipe & re-provision Partition 4 (Data)
  ./iris status              Show built images, ZBIs & Data partition status
  ./iris doctor              Verify Homebrew LLVM, QEMU, mtools & assets
  ./iris graph [query]       Regenerate or query architecture/symbol graph
  ./iris clean               Clean build outputs (preserves disk_*.raw)

TARGET ARCHITECTURES:
  all                        Both x86_64 and aarch64 architectures
  x86_64                     64-bit x86_64 architecture (UEFI q35)
  aarch64                    64-bit AArch64 architecture (UEFI virt)

EXAMPLES:
  ./iris build all           Build both architectures and refresh repo_graph
  ./iris run x86_64          Boot x86_64 VM (auto-builds if missing)
  ./iris reset-data x86_64   Prompted wipe & fresh encrypted Data partition
  ./iris doctor              Check toolchain and bundled fonts/wallpapers

(You can also invoke 'make build all', 'make run x86_64', 'make doctor', etc.)
======================================================================
EOF
}

resolve_arches() {
    local target="${1:-}"
    case "${target}" in
        all)     echo "x86_64 aarch64" ;;
        x86_64)  echo "x86_64" ;;
        aarch64) echo "aarch64" ;;
        "")
            log_err "Missing architecture target (expected: all | x86_64 | aarch64)."
            exit 1
            ;;
        *)
            log_err "Unknown architecture target '${target}' (expected: all | x86_64 | aarch64)."
            exit 1
            ;;
    esac
}

human_size() {
    local file="$1"
    if [[ ! -f "${file}" ]]; then
        echo "missing"
        return
    fi
    ls -lh "${file}" | awk '{print $5}'
}

cmd_build() {
    local target="${1:-}"
    local arches
    arches="$(resolve_arches "${target}")"

    banner "[irisOS] Building target(s): ${target}"
    local arch
    for arch in ${arches}; do
        log_info "Building ${arch}..."
        make -C "${BUILD_DIR}" "image-${arch}"
        log_ok "${arch} image ready: ${IMAGES_DIR}/disk_${arch}.raw"
    done

    python3 "${SCRIPT_DIR}/gen_repo_graph.py"
}

cmd_run() {
    local target="${1:-}"
    if [[ -z "${target}" ]]; then
        log_err "Missing architecture target for 'run'."
        echo "Usage: ./iris run [all | x86_64 | aarch64]"
        exit 1
    fi
    shift || true

    if [[ "${target}" == "all" ]]; then
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
            log_info "Operation cancelled by user."
            exit 0
        fi

        local arch
        for arch in x86_64 aarch64; do
            if [[ ! -f "${IMAGES_DIR}/disk_${arch}.raw" ]]; then
                log_info "${arch} image not found. Building first..."
                make -C "${BUILD_DIR}" "image-${arch}"
            fi
        done

        log_info "Launching dual VMs (Ctrl+C to terminate both)..."
        "${SCRIPT_DIR}/run_qemu.sh" -a x86_64 2>&1 | awk '{ print "[x86_64]  " $0; fflush(); }' &
        local pid_x86=$!
        "${SCRIPT_DIR}/run_qemu.sh" -a aarch64 2>&1 | awk '{ print "[aarch64] " $0; fflush(); }' &
        local pid_arm=$!

        cleanup_dual() {
            echo ""
            log_info "Shutting down dual QEMU instances (${pid_x86}, ${pid_arm})..."
            kill -TERM "${pid_x86}" "${pid_arm}" 2>/dev/null || true
            wait "${pid_x86}" "${pid_arm}" 2>/dev/null || true
        }
        trap cleanup_dual INT TERM EXIT
        wait "${pid_x86}" "${pid_arm}" 2>/dev/null || true
        return
    fi

    local arch
    arch="$(resolve_arches "${target}")"
    if [[ ! -f "${IMAGES_DIR}/disk_${arch}.raw" ]]; then
        log_info "${arch} disk image not found. Building first..."
        make -C "${BUILD_DIR}" "image-${arch}"
    fi
    exec "${SCRIPT_DIR}/run_qemu.sh" -a "${arch}" "$@"
}

cmd_reset_data() {
    local target="${1:-}"
    local arches
    arches="$(resolve_arches "${target}")"

    echo "${C_RED}${C_BOLD}"
    cat << 'EOF'
======================================================================
  DANGER: DESTRUCTIVE DATA PARTITION RESET
======================================================================
  This operation will PERMANENTLY ERASE Partition 4 (Data) for:
EOF
    echo "    -> ${target}"
    cat << 'EOF'

  All user encrypted vaults (/Vaults/*.img), saved files under /Users,
  taskbar pin layouts, and installed /Applications data WILL BE LOST.
  A clean Data partition with default dev accounts (Administrator +
  Guest) will be formatted in its place.
======================================================================
EOF
    echo -n "${C_RESET}"

    local confirm=""
    read -r -p "Type 'RESET' in all caps to proceed (anything else aborts): " confirm || confirm=""
    if [[ "${confirm}" != "RESET" ]]; then
        log_info "Aborted. Existing Data partition(s) left untouched."
        exit 0
    fi

    local arch
    for arch in ${arches}; do
        log_info "Re-provisioning Partition 4 (Data) for ${arch}..."
        IRIS_RESET_DATA=1 make -C "${BUILD_DIR}" "image-${arch}"
        log_ok "Reset Data partition in ${IMAGES_DIR}/disk_${arch}.raw"
    done
}

cmd_status() {
    banner "irisOS Build & Image Status"
    local arch
    for arch in x86_64 aarch64; do
        local zbi="${ROOT_DIR}/out/build-${arch}/iris_a.zbi"
        local esp="${IMAGES_DIR}/esp_${arch}.img"
        local disk="${IMAGES_DIR}/disk_${arch}.raw"
        echo "${C_BOLD}${arch}:${C_RESET}"
        printf "  %-18s %s\n" "iris_a.zbi:" "$(human_size "${zbi}")"
        printf "  %-18s %s\n" "esp_${arch}.img:" "$(human_size "${esp}")"
        printf "  %-18s %s\n" "disk_${arch}.raw:" "$(human_size "${disk}")"
    done
}

cmd_doctor() {
    banner "irisOS Environment & Asset Check"
    local missing=0
    local llvm_bin="/opt/homebrew/opt/llvm/bin"

    check_bin() {
        local label="$1" path="$2"
        if command -v "${path}" >/dev/null 2>&1 || [[ -x "${path}" ]]; then
            log_ok "${label} (${path})"
        else
            log_err "Missing ${label} (${path})"
            missing=$((missing + 1))
        fi
    }

    check_bin "Homebrew clang"        "${llvm_bin}/clang"
    check_bin "Homebrew ld.lld"       "ld.lld"
    check_bin "cmake"                 "cmake"
    check_bin "ninja"                 "ninja"
    check_bin "qemu-system-x86_64"    "qemu-system-x86_64"
    check_bin "qemu-system-aarch64"   "qemu-system-aarch64"
    check_bin "mformat"               "/opt/homebrew/bin/mformat"
    check_bin "mcopy"                 "/opt/homebrew/bin/mcopy"
    check_bin "python3"               "python3"

    local assets=(
        "resources/fonts/Inter-Variable.ttf"
        "resources/fonts/Inter-Italic-Variable.ttf"
        "resources/fonts/InterTight-Variable.ttf"
        "resources/fonts/InterTight-Italic-Variable.ttf"
        "resources/fonts/JetBrainsMono-Variable.ttf"
        "resources/fonts/JetBrainsMono-Italic-Variable.ttf"
        "resources/fonts/InstrumentSerif-Regular.ttf"
        "resources/fonts/InstrumentSerif-Italic.ttf"
        "resources/fonts/MaterialIcons-Regular.ttf"
        "resources/icons/eye_light.svg"
        "resources/icons/eye_dark.svg"
        "resources/wallpaper/wallpaper.qoi"
        "resources/wallpaper/wallpaper_admin.qoi"
        "resources/wallpaper/wallpaper_guest.qoi"
        "resources/avatars/avatar_admin.qoi"
    )
    local rel
    for rel in "${assets[@]}"; do
        if [[ -f "${ROOT_DIR}/${rel}" ]]; then
            log_ok "Asset ${rel} ($(human_size "${ROOT_DIR}/${rel}"))"
        else
            log_warn "Missing asset ${rel}"
        fi
    done

    if [[ ${missing} -ne 0 ]]; then
        log_err "${missing} required host tool(s) missing. Install via: brew install llvm cmake ninja meson qemu xorriso mtools"
        exit 1
    fi
}

cmd_clean() {
    log_info "Cleaning build outputs (preserving disk_*.raw Data partitions)..."
    make -C "${BUILD_DIR}" clean
}

cmd_graph() {
    python3 "${SCRIPT_DIR}/gen_repo_graph.py" "$@"
}

COMMAND="${1:-help}"
shift || true

case "${COMMAND}" in
    build)       cmd_build "$@" ;;
    run)         cmd_run "$@" ;;
    reset-data)  cmd_reset_data "$@" ;;
    status)      cmd_status "$@" ;;
    doctor)      cmd_doctor "$@" ;;
    graph)       cmd_graph "$@" ;;
    clean)       cmd_clean "$@" ;;
    help|-h|--help) show_help ;;
    *)
        log_err "Unknown command '${COMMAND}'."
        show_help
        exit 1
        ;;
esac

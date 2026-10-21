#!/usr/bin/env bash
# ==============================================================================
# irisOS - Toybox Multicall Userland Cross-Compilation Script
# ==============================================================================
# Cross-compiles Toybox (sh + POSIX utilities) for x86_64 and aarch64
# using Homebrew LLVM against the irisOS mlibc sysroot.
# ==============================================================================

set -euo pipefail

ARCH="${1:-x86_64}"
OUT_DIR="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOYBOX_SRC="${ROOT_DIR}/vendor/toybox"
COMPAT_DIR="${ROOT_DIR}/build/toybox_compat"

if [[ -z "${OUT_DIR}" ]]; then
    OUT_DIR="${ROOT_DIR}/out/build-${ARCH}"
fi

case "${ARCH}" in
    x86_64)
        TARGET_TRIPLE="x86_64-unknown-elf"
        CRT_ARCH="x86_64"
        ;;
    aarch64)
        TARGET_TRIPLE="aarch64-unknown-elf"
        CRT_ARCH="aarch64"
        ;;
    *)
        echo "Error: Unsupported architecture '${ARCH}'. Use 'x86_64' or 'aarch64'."
        exit 1
        ;;
esac

# 1. Ensure Toybox sources are fetched
if [[ ! -f "${TOYBOX_SRC}/main.c" || ! -f "${TOYBOX_SRC}/Config.in" ]]; then
    echo "==> Fetching Toybox sources..."
    "${ROOT_DIR}/scripts/fetch_toybox.sh"
fi

# 2. Ensure generated headers exist in vendor/toybox/generated
if [[ ! -f "${TOYBOX_SRC}/generated/config.h" || ! -f "${TOYBOX_SRC}/generated/newtoys.h" || ! -f "${TOYBOX_SRC}/generated/flags.h" ]]; then
    echo "==> Generating Toybox configuration and headers..."
    (
        cd "${TOYBOX_SRC}"
        make -C . kconfig/conf
        scripts/genconfig.sh
        kconfig/conf -n Config.in >/dev/null
        python3 -c "
with open('.config') as f:
    lines = f.readlines()

enabled = {'CAT', 'CLEAR', 'CP', 'ECHO', 'GREP', 'KILL', 'LS', 'MKDIR', 'PWD', 'RM', 'SH', 'SLEEP', 'TOUCH', 'UNAME', 'WC',
           'TOYBOX', 'TOYBOX_SUID', 'TOYBOX_LSM_NONE', 'TOYBOX_FLOAT', 'TOYBOX_HELP', 'TOYBOX_HELP_DASHDASH', 'TOYBOX_FORCE_NOMMU'}

seen = set()
new_lines = []
for line in lines:
    s = line.strip()
    if s.startswith('CONFIG_') or s.startswith('# CONFIG_'):
        sym = s.split('=')[0].replace('CONFIG_', '') if '=' in s else s.split()[1].replace('CONFIG_', '')
        if sym in seen:
            continue
        seen.add(sym)
        if sym in enabled:
            new_lines.append(f'CONFIG_{sym}=y\n')
        elif sym == 'TOYBOX_FORK':
            new_lines.append('# CONFIG_TOYBOX_FORK is not set\n')
        elif sym == 'TOYBOX_UID_SYS':
            new_lines.append('CONFIG_TOYBOX_UID_SYS=100\n')
        elif sym == 'TOYBOX_UID_USR':
            new_lines.append('CONFIG_TOYBOX_UID_USR=500\n')
        else:
            new_lines.append(f'# CONFIG_{sym} is not set\n')

with open('.config', 'w') as f:
    f.writelines(new_lines)
"
        kconfig/conf -s Config.in </dev/null >/dev/null
        SED=gsed scripts/make.sh || true
    )
fi

LLVM_PREFIX="/opt/homebrew/opt/llvm"
CLANG="${LLVM_PREFIX}/bin/clang"
LLD="${LLVM_PREFIX}/bin/ld.lld"
STRIP="${LLVM_PREFIX}/bin/llvm-strip"
CLANG_RES="$("${CLANG}" -print-resource-dir)"

OBJ_DIR="${OUT_DIR}/toybox-objs"
mkdir -p "${OBJ_DIR}" "${OUT_DIR}/bin"

COMMON_CFLAGS=(
    "--target=${TARGET_TRIPLE}"
    "-O2"
    "-fPIE"
    "-nostdinc"
    "-D__linux__=1"
    "-D_GNU_SOURCE"
    "-Wno-invalid-source-encoding"
    "-Wno-tautological-constant-out-of-range-compare"
    "-Wno-string-plus-int"
    "-Wno-unused-variable"
    "-Wno-unused-function"
    "-isystem" "${CLANG_RES}/include"
    "-isystem" "${COMPAT_DIR}"
    "-isystem" "${OUT_DIR}/mlibc"
    "-isystem" "${ROOT_DIR}/sdk/mlibc-sysdeps/iris/include"
    "-isystem" "${ROOT_DIR}/sdk/libiris/include"
    "-isystem" "${ROOT_DIR}"
    "-isystem" "${ROOT_DIR}/kernel/zircon/system/public"
    "-isystem" "${OUT_DIR}/gen"
    "-isystem" "${OUT_DIR}/gen/zircon"
    "-isystem" "${ROOT_DIR}/vendor/mlibc/options/ansi/include"
    "-isystem" "${ROOT_DIR}/vendor/mlibc/options/posix/include"
    "-isystem" "${ROOT_DIR}/vendor/mlibc/options/bsd/include"
    "-isystem" "${ROOT_DIR}/vendor/mlibc/options/glibc/include"
    "-isystem" "${ROOT_DIR}/vendor/mlibc/options/linux/include"
    "-isystem" "${ROOT_DIR}/vendor/mlibc/options/internal/include"
    "-isystem" "${ROOT_DIR}/vendor/mlibc/include"
    "-I${TOYBOX_SRC}"
    "-I${TOYBOX_SRC}/generated"
)

SOURCES=(
    "${TOYBOX_SRC}/main.c"
    "${COMPAT_DIR}/stubs.c"
    "${TOYBOX_SRC}/lib/args.c"
    "${TOYBOX_SRC}/lib/commas.c"
    "${TOYBOX_SRC}/lib/deflate.c"
    "${TOYBOX_SRC}/lib/dirtree.c"
    "${TOYBOX_SRC}/lib/elf.c"
    "${TOYBOX_SRC}/lib/env.c"
    "${TOYBOX_SRC}/lib/hash.c"
    "${TOYBOX_SRC}/lib/lib.c"
    "${TOYBOX_SRC}/lib/llist.c"
    "${TOYBOX_SRC}/lib/net.c"
    "${TOYBOX_SRC}/lib/password.c"
    "${TOYBOX_SRC}/lib/portability.c"
    "${TOYBOX_SRC}/lib/tty.c"
    "${TOYBOX_SRC}/lib/utf8.c"
    "${TOYBOX_SRC}/lib/xwrap.c"
    "${TOYBOX_SRC}/toys/posix/cat.c"
    "${TOYBOX_SRC}/toys/posix/cp.c"
    "${TOYBOX_SRC}/toys/posix/echo.c"
    "${TOYBOX_SRC}/toys/posix/grep.c"
    "${TOYBOX_SRC}/toys/posix/kill.c"
    "${TOYBOX_SRC}/toys/posix/ls.c"
    "${TOYBOX_SRC}/toys/posix/mkdir.c"
    "${TOYBOX_SRC}/toys/posix/pwd.c"
    "${TOYBOX_SRC}/toys/posix/rm.c"
    "${TOYBOX_SRC}/toys/posix/sleep.c"
    "${TOYBOX_SRC}/toys/posix/test.c"
    "${TOYBOX_SRC}/toys/posix/touch.c"
    "${TOYBOX_SRC}/toys/posix/uname.c"
    "${TOYBOX_SRC}/toys/posix/wc.c"
    "${TOYBOX_SRC}/toys/pending/sh.c"
    "${TOYBOX_SRC}/toys/other/clear.c"
)

OBJS=()
echo "==> Compiling Toybox (${ARCH})..."

for src in "${SOURCES[@]}"; do
    base="$(basename "${src}" .c)"
    dir="$(basename "$(dirname "${src}")")"
    obj="${OBJ_DIR}/${dir}_${base}.o"
    OBJS+=("${obj}")

    if [[ ! -f "${obj}" || "${src}" -nt "${obj}" ]]; then
        "${CLANG}" "${COMMON_CFLAGS[@]}" -c "${src}" -o "${obj}"
    fi
done

TOYBOX_BIN="${OUT_DIR}/bin/toybox"
echo "==> Linking Toybox (${TOYBOX_BIN})..."

"${CLANG}" \
    "--target=${TARGET_TRIPLE}" \
    "-nostdlib" \
    "-fuse-ld=lld" \
    "-B${LLVM_PREFIX}/bin" \
    "-Wl,-pie" \
    "-Wl,-z,text" \
    "${OUT_DIR}/mlibc/sysdeps/iris/crt1.o" \
    "${OUT_DIR}/mlibc/sysdeps/iris/crti.o" \
    "${OBJS[@]}" \
    "-L${OUT_DIR}/mlibc" "-lc" \
    "-L${OUT_DIR}/lib" "-liris" "-lclang_rt.builtins-${CRT_ARCH}" \
    "${OUT_DIR}/mlibc/sysdeps/iris/crtn.o" \
    "-o" "${TOYBOX_BIN}"

"${STRIP}" -s "${TOYBOX_BIN}"

echo "    [DONE] Toybox binary created: ${TOYBOX_BIN}"

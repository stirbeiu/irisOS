#!/usr/bin/env bash
# ==============================================================================
# irisOS - Variable Fonts & Material Icons Fetch Script
# ==============================================================================
# Downloads permissive OFL-1.1 variable fonts (Inter / Inter Display,
# Inter Tight, JetBrains Mono) and Apache-2.0 Material Icons into
# resources/fonts/.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FONTS_DIR="${REPO_ROOT}/resources/fonts"

mkdir -p "${FONTS_DIR}"

INTER_ZIP_URL="https://github.com/rsms/inter/releases/download/v4.0/Inter-4.0.zip"
INTER_TIGHT_REG_URL="https://raw.githubusercontent.com/google/fonts/main/ofl/intertight/InterTight%5Bwght%5D.ttf"
INTER_TIGHT_IT_URL="https://raw.githubusercontent.com/google/fonts/main/ofl/intertight/InterTight-Italic%5Bwght%5D.ttf"
JB_MONO_REG_URL="https://raw.githubusercontent.com/google/fonts/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf"
JB_MONO_IT_URL="https://raw.githubusercontent.com/google/fonts/main/ofl/jetbrainsmono/JetBrainsMono-Italic%5Bwght%5D.ttf"
MATERIAL_ICONS_URL="https://raw.githubusercontent.com/google/material-design-icons/master/font/MaterialIcons-Regular.ttf"

if [[ "${1:-}" == "--clean" ]]; then
    rm -f \
        "${FONTS_DIR}/Inter-Variable.ttf" \
        "${FONTS_DIR}/Inter-Italic-Variable.ttf" \
        "${FONTS_DIR}/InterDisplay-Variable.ttf" \
        "${FONTS_DIR}/InterDisplay-Italic-Variable.ttf" \
        "${FONTS_DIR}/InterTight-Variable.ttf" \
        "${FONTS_DIR}/InterTight-Italic-Variable.ttf" \
        "${FONTS_DIR}/JetBrainsMono-Variable.ttf" \
        "${FONTS_DIR}/JetBrainsMono-Italic-Variable.ttf" \
        "${FONTS_DIR}/MaterialIcons-Regular.ttf"
fi

TEMP_DIR="$(mktemp -d /tmp/iris_fonts_fetch.XXXXXX)"
cleanup() {
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

if [[ ! -f "${FONTS_DIR}/Inter-Variable.ttf" || ! -f "${FONTS_DIR}/Inter-Italic-Variable.ttf" ]]; then
    echo "==> Downloading Inter v4.0 variable fonts..."
    curl -fsSL -o "${TEMP_DIR}/Inter-4.0.zip" "${INTER_ZIP_URL}"
    mkdir -p "${TEMP_DIR}/inter_unpack"
    unzip -q -o "${TEMP_DIR}/Inter-4.0.zip" -d "${TEMP_DIR}/inter_unpack"
    INTER_VAR_REG="$(find "${TEMP_DIR}/inter_unpack" -name "InterVariable.ttf" -type f | head -n 1)"
    INTER_VAR_IT="$(find "${TEMP_DIR}/inter_unpack" -name "InterVariable-Italic.ttf" -type f | head -n 1)"
    if [[ -z "${INTER_VAR_REG}" || -z "${INTER_VAR_IT}" ]]; then
        echo "ERROR: Could not locate InterVariable*.ttf in Inter-4.0.zip" >&2
        exit 1
    fi
    cp -f "${INTER_VAR_REG}" "${FONTS_DIR}/Inter-Variable.ttf"
    cp -f "${INTER_VAR_IT}" "${FONTS_DIR}/Inter-Italic-Variable.ttf"
fi

if [[ ! -f "${FONTS_DIR}/InterTight-Variable.ttf" ]]; then
    echo "==> Downloading InterTight-Variable.ttf..."
    curl -fsSL -o "${FONTS_DIR}/InterTight-Variable.ttf" "${INTER_TIGHT_REG_URL}"
fi
if [[ ! -f "${FONTS_DIR}/InterTight-Italic-Variable.ttf" ]]; then
    echo "==> Downloading InterTight-Italic-Variable.ttf..."
    curl -fsSL -o "${FONTS_DIR}/InterTight-Italic-Variable.ttf" "${INTER_TIGHT_IT_URL}"
fi

if [[ ! -f "${FONTS_DIR}/JetBrainsMono-Variable.ttf" ]]; then
    echo "==> Downloading JetBrainsMono-Variable.ttf..."
    curl -fsSL -o "${FONTS_DIR}/JetBrainsMono-Variable.ttf" "${JB_MONO_REG_URL}"
fi
if [[ ! -f "${FONTS_DIR}/JetBrainsMono-Italic-Variable.ttf" ]]; then
    echo "==> Downloading JetBrainsMono-Italic-Variable.ttf..."
    curl -fsSL -o "${FONTS_DIR}/JetBrainsMono-Italic-Variable.ttf" "${JB_MONO_IT_URL}"
fi

if [[ ! -f "${FONTS_DIR}/MaterialIcons-Regular.ttf" ]]; then
    echo "==> Downloading MaterialIcons-Regular.ttf..."
    curl -fsSL -o "${FONTS_DIR}/MaterialIcons-Regular.ttf" "${MATERIAL_ICONS_URL}"
fi

echo "==> Font assets ready in ${FONTS_DIR}."

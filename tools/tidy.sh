#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run clang-tidy over first-party targets using a preset's compile_commands.json.
#
#   tools/tidy.sh [preset]     default preset: macos-debug
#
# Third-party headers are excluded: vcpkg builds those separately and their diagnostics are
# not actionable here.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT

PRESET="${1:-macos-debug}"
BUILD_DIR="${REPO_ROOT}/build/${PRESET}"
COMPILE_DB="${BUILD_DIR}/compile_commands.json"

if [[ ! -f "${COMPILE_DB}" ]]; then
    echo "error: ${COMPILE_DB} not found." >&2
    echo "  Configure first: cmake --preset ${PRESET}" >&2
    exit 1
fi

find_tool() {
    local name="$1"
    if [[ -n "${ATLAS_CLANG_TIDY:-}" && "${name}" == "clang-tidy" ]]; then
        echo "${ATLAS_CLANG_TIDY}"
        return
    fi
    for candidate in \
        "/opt/homebrew/opt/llvm/bin/${name}" \
        "/usr/local/opt/llvm/bin/${name}" \
        "$(command -v "${name}" 2>/dev/null || true)"
    do
        if [[ -n "${candidate}" && -x "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done
    echo ""
}

CLANG_TIDY_BIN="$(find_tool clang-tidy)"
if [[ -z "${CLANG_TIDY_BIN}" ]]; then
    echo "error: clang-tidy not found. Install LLVM, or set ATLAS_CLANG_TIDY." >&2
    exit 1
fi

RUN_CLANG_TIDY_BIN="$(find_tool run-clang-tidy)"

# Homebrew's clang-tidy does not know where Apple's SDK lives, so without an explicit
# sysroot it fails to find <cstdint> and friends. A broken parse produces a flood of
# cascading false positives rather than an honest error, so this matters.
RUN_EXTRA_ARGS=()
TIDY_EXTRA_ARGS=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [[ -n "${SDK_PATH}" ]]; then
        RUN_EXTRA_ARGS+=("-extra-arg=-isysroot" "-extra-arg=${SDK_PATH}")
        TIDY_EXTRA_ARGS+=("--extra-arg=-isysroot" "--extra-arg=${SDK_PATH}")
    fi
fi

FILES=()
while IFS= read -r file; do
    [[ -n "${file}" ]] && FILES+=("${file}")
done < <(
    find "${REPO_ROOT}/engine" "${REPO_ROOT}/apps" \
         -type f \( -name '*.cpp' \) \
         -not -path '*/tests/*' 2>/dev/null | sort
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "no source files to analyse"
    exit 0
fi

echo "clang-tidy: $("${CLANG_TIDY_BIN}" --version | head -2 | tail -1)"
echo "analysing ${#FILES[@]} file(s) from preset '${PRESET}'"

if [[ -n "${RUN_CLANG_TIDY_BIN}" ]]; then
    # run-clang-tidy parallelises; -quiet suppresses the per-file banner.
    "${RUN_CLANG_TIDY_BIN}" \
        -p "${BUILD_DIR}" \
        -clang-tidy-binary "${CLANG_TIDY_BIN}" \
        -quiet \
        "${RUN_EXTRA_ARGS[@]}" \
        "${FILES[@]}"
else
    for file in "${FILES[@]}"; do
        "${CLANG_TIDY_BIN}" -p "${BUILD_DIR}" "${TIDY_EXTRA_ARGS[@]}" "${file}"
    done
fi

echo "clang-tidy clean"

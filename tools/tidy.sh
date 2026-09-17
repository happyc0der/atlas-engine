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

# The wrapper must match the binary it drives. A version 19 run-clang-tidy driving a version 23
# clang-tidy resolved the configuration to nothing at all and reported "No checks enabled", which
# is a linter that passes by doing nothing — the worst possible failure for one. So the wrapper is
# derived from whatever clang-tidy was chosen, rather than whatever happens to be first on PATH,
# and if the matching one is missing this falls back to calling clang-tidy per file: slower, and
# right.
RUN_CLANG_TIDY_BIN=""
if [[ -n "${ATLAS_RUN_CLANG_TIDY:-}" ]]; then
    RUN_CLANG_TIDY_BIN="${ATLAS_RUN_CLANG_TIDY}"
elif [[ "${CLANG_TIDY_BIN}" =~ ^(.*/)?clang-tidy(-[0-9]+)?$ ]]; then
    sibling="$(dirname "${CLANG_TIDY_BIN}")/run-clang-tidy${BASH_REMATCH[2]:-}"
    if [[ -x "${sibling}" ]]; then
        RUN_CLANG_TIDY_BIN="${sibling}"
    fi
fi

if [[ -n "${RUN_CLANG_TIDY_BIN}" ]]; then
    echo "run-clang-tidy: ${RUN_CLANG_TIDY_BIN}"
else
    echo "run-clang-tidy: none matching ${CLANG_TIDY_BIN}; analysing one file at a time"
fi

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

# Two passes over two sets of files.
#
# Production sources take the repository's configuration. Tests and benchmarks take
# tools/clang-tidy-tests.yml, which inherits it and then disables the handful of checks that a
# testing framework makes meaningless; the reasons are in that file. They were analysed by
# nothing at all until M8, which is how several hundred findings accumulated unseen in code
# that is as much a part of the project as the rest of it.
PRODUCTION_FILES=()
while IFS= read -r file; do
    [[ -n "${file}" ]] && PRODUCTION_FILES+=("${file}")
done < <(
    find "${REPO_ROOT}/engine" "${REPO_ROOT}/apps" \
         -type f \( -name '*.cpp' \) \
         -not -path '*/tests/*' 2>/dev/null | sort
)

TEST_FILES=()
while IFS= read -r file; do
    [[ -n "${file}" ]] && TEST_FILES+=("${file}")
done < <(
    {
        find "${REPO_ROOT}/engine" "${REPO_ROOT}/apps" \
             -type f \( -name '*.cpp' \) \
             -path '*/tests/*' 2>/dev/null
        find "${REPO_ROOT}/benchmarks" -type f \( -name '*.cpp' \) 2>/dev/null
    } | sort
)

if [[ ${#PRODUCTION_FILES[@]} -eq 0 && ${#TEST_FILES[@]} -eq 0 ]]; then
    echo "no source files to analyse"
    exit 0
fi

echo "clang-tidy: $("${CLANG_TIDY_BIN}" --version | head -2 | tail -1)"
echo "analysing ${#PRODUCTION_FILES[@]} production and ${#TEST_FILES[@]} test file(s) from preset '${PRESET}'"

# analyse <config-file-or-empty> <file>...
analyse() {
    local config="$1"
    shift
    [[ $# -eq 0 ]] && return 0

    local config_args=()
    [[ -n "${config}" ]] && config_args+=("-config-file=${config}")

    # ${a[@]+"${a[@]}"} rather than "${a[@]}": under `set -u` the macOS system bash, which is
    # still 3.2, treats expanding an empty array as an unbound variable where bash 5 on the
    # continuous-integration image does not. The production pass has always had a non-empty
    # extra-argument array on macOS, so this only became visible when a pass without one
    # arrived.
    if [[ -n "${RUN_CLANG_TIDY_BIN}" ]]; then
        # run-clang-tidy parallelises; -quiet suppresses the per-file banner.
        "${RUN_CLANG_TIDY_BIN}" \
            -p "${BUILD_DIR}" \
            -clang-tidy-binary "${CLANG_TIDY_BIN}" \
            -quiet \
            ${config_args[@]+"${config_args[@]}"} \
            ${RUN_EXTRA_ARGS[@]+"${RUN_EXTRA_ARGS[@]}"} \
            "$@"
    else
        local file
        for file in "$@"; do
            "${CLANG_TIDY_BIN}" -p "${BUILD_DIR}" \
                ${config_args[@]+"${config_args[@]}"} \
                ${TIDY_EXTRA_ARGS[@]+"${TIDY_EXTRA_ARGS[@]}"} "${file}"
        done
    fi
}

analyse "" "${PRODUCTION_FILES[@]}"
analyse "${REPO_ROOT}/tools/clang-tidy-tests.yml" "${TEST_FILES[@]}"

echo "clang-tidy clean"

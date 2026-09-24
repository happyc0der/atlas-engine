#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything that must be green before a slice is reported complete.
#
#   tools/precheck.sh [preset]     default preset: macos-debug
#
# Ordered cheapest first, so a trivial mistake fails in a second rather than after a build.
# clang-tidy is skipped unless ATLAS_PRECHECK_TIDY=1, because it roughly doubles build time
# and the lint CI job covers it.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
cd "${REPO_ROOT}"

PRESET="${1:-macos-debug}"
FAILURES=0

step() {
    local name="$1"
    shift
    echo ""
    echo "─── ${name} ───"
    if "$@"; then
        echo "✓ ${name}"
    else
        echo "✗ ${name}" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

step "SPDX headers"      python3 tools/check_spdx.py
step "Module boundaries" python3 tools/check_module_deps.py
step "vcpkg pin"         python3 tools/check_vcpkg_pin.py
step "Formatting"        ./tools/format.sh --check
step "Configure"         cmake --preset "${PRESET}"
step "Build"             cmake --build --preset "${PRESET}"
step "Tests"             ctest --preset "${PRESET}"

if [[ "${ATLAS_PRECHECK_TIDY:-0}" == "1" ]]; then
    step "clang-tidy" ./tools/tidy.sh "${PRESET}"
fi

echo ""
if [[ ${FAILURES} -eq 0 ]]; then
    echo "precheck passed (preset: ${PRESET})"
    exit 0
fi

echo "precheck FAILED: ${FAILURES} step(s) (preset: ${PRESET})" >&2
exit 1

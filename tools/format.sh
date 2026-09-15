#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Format, or check formatting of, first-party C++ sources.
#
#   tools/format.sh --check    verify only; non-zero exit when a file would change
#   tools/format.sh --fix      rewrite files in place
#
# clang-format output is not stable across major versions, so the major is pinned here.
# A different major would reformat the whole tree and make every diff unreadable.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly REQUIRED_MAJOR=23

usage() {
    echo "usage: tools/format.sh [--check|--fix]" >&2
    exit 2
}

MODE="check"
case "${1:---check}" in
    --check) MODE="check" ;;
    --fix)   MODE="fix" ;;
    -h|--help) usage ;;
    *) usage ;;
esac

# Find clang-format: an explicit override, then Homebrew LLVM (which is not on PATH on the
# development machine), then whatever is on PATH.
find_clang_format() {
    if [[ -n "${CLANG_FORMAT:-}" ]]; then
        echo "${CLANG_FORMAT}"
        return
    fi
    for candidate in \
        "/opt/homebrew/opt/llvm/bin/clang-format" \
        "/usr/local/opt/llvm/bin/clang-format" \
        "$(command -v clang-format-${REQUIRED_MAJOR} 2>/dev/null || true)" \
        "$(command -v clang-format 2>/dev/null || true)"
    do
        if [[ -n "${candidate}" && -x "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done
    echo ""
}

CLANG_FORMAT_BIN="$(find_clang_format)"
if [[ -z "${CLANG_FORMAT_BIN}" ]]; then
    echo "error: clang-format not found." >&2
    echo "  Install LLVM (brew install llvm), or set CLANG_FORMAT to its path." >&2
    exit 1
fi

VERSION_LINE="$("${CLANG_FORMAT_BIN}" --version)"
MAJOR="$(echo "${VERSION_LINE}" | sed -E 's/.*version ([0-9]+).*/\1/')"
if [[ "${MAJOR}" != "${REQUIRED_MAJOR}" ]]; then
    echo "error: clang-format major ${MAJOR} found, ${REQUIRED_MAJOR} required." >&2
    echo "  ${VERSION_LINE}" >&2
    echo "  Formatting differs between majors; a mismatch rewrites the whole tree." >&2
    echo "  Set CLANG_FORMAT to a version ${REQUIRED_MAJOR} binary." >&2
    exit 1
fi

# A read loop rather than mapfile: macOS ships bash 3.2, where mapfile does not exist.
FILES=()
while IFS= read -r file; do
    [[ -n "${file}" ]] && FILES+=("${file}")
done < <(
    find "${REPO_ROOT}/engine" "${REPO_ROOT}/apps" "${REPO_ROOT}/tests" \
         "${REPO_ROOT}/benchmarks" \
         -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.inl' \) \
         2>/dev/null | sort
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "no source files found"
    exit 0
fi

if [[ "${MODE}" == "fix" ]]; then
    "${CLANG_FORMAT_BIN}" -i --style=file "${FILES[@]}"
    echo "formatted ${#FILES[@]} file(s) with $(basename "${CLANG_FORMAT_BIN}") ${MAJOR}"
    exit 0
fi

if "${CLANG_FORMAT_BIN}" --dry-run --Werror --style=file "${FILES[@]}"; then
    echo "formatting clean: ${#FILES[@]} file(s)"
    exit 0
fi

echo "" >&2
echo "error: formatting differences above. Run: tools/format.sh --fix" >&2
exit 1

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PostToolUse hook: format a C++ file immediately after it is edited.
#
# Formatting one touched file is fast; formatting the tree is not. Anything that is not a
# first-party C++ source is left alone. A missing or mismatched clang-format is not an
# error here: tools/format.sh --check is the gate, and it reports the problem properly.
set -uo pipefail

PAYLOAD="$(cat)"

FILE="$(printf '%s' "${PAYLOAD}" | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
path = data.get("tool_input", {}).get("file_path", "")
print(path)
' 2>/dev/null)"

[[ -z "${FILE}" || ! -f "${FILE}" ]] && exit 0

case "${FILE}" in
    *.cpp|*.hpp|*.h|*.inl) ;;
    *) exit 0 ;;
esac

# First-party only: never reformat vendored or generated code.
case "${FILE}" in
    */external/*|*/build/*|*/vcpkg_installed/*|*/generated/*) exit 0 ;;
esac

for candidate in \
    "${CLANG_FORMAT:-}" \
    "/opt/homebrew/opt/llvm/bin/clang-format" \
    "/usr/local/opt/llvm/bin/clang-format"
do
    if [[ -n "${candidate}" && -x "${candidate}" ]]; then
        "${candidate}" -i --style=file "${FILE}" 2>/dev/null || true
        exit 0
    fi
done

exit 0

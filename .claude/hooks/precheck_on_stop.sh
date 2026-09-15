#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stop hook: refuse to finish on a tree that does not build and pass.
#
# CLAUDE.md's completion checklist says a slice is done only when precheck is clean. A
# checklist that depends on remembering it is not a checklist, so this runs it. Exit code 2
# blocks the stop and hands the output back, which is the point: the failure is reported at
# the moment it could still be fixed rather than in the next session.
#
# Two things keep it from being a nuisance.
#
# It skips when nothing that could affect the build has changed. A turn that edited only
# documentation does not need a compile, and paying a minute for one would train everybody to
# disable the hook.
#
# It never blocks twice in a row. Claude Code sets stop_hook_active when a stop has already
# been blocked once; blocking again on the same failure would be a loop, so the second time
# it reports and gets out of the way. A failure that needs several attempts is a conversation,
# not something a hook should force.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly REPO_ROOT
cd "${REPO_ROOT}" || exit 0

PRESET="${ATLAS_PRECHECK_PRESET:-macos-debug}"
MARKER="build/.precheck-state"

PAYLOAD="$(cat 2>/dev/null || true)"

# Already blocked once this turn. Reporting again would loop.
ALREADY_BLOCKED="$(printf '%s' "${PAYLOAD}" | python3 -c '
import json, sys
try:
    print("1" if json.load(sys.stdin).get("stop_hook_active") else "0")
except Exception:
    print("0")
' 2>/dev/null || echo 0)"

if [[ "${ALREADY_BLOCKED}" == "1" ]]; then
    exit 0
fi

# A fingerprint of everything that could change the outcome: the commit, and the content of
# every tracked build-relevant file plus any untracked ones. Content rather than timestamps,
# so reformatting a file back to how it was does not count as a change.
fingerprint() {
    {
        git rev-parse HEAD 2>/dev/null || echo "no-commit"
        git status --porcelain 2>/dev/null
        git ls-files -z -- \
            '*.cpp' '*.hpp' '*.h' '*.inl' '*.cmake' 'CMakeLists.txt' '**/CMakeLists.txt' \
            'CMakePresets.json' 'vcpkg.json' '*.py' '*.hlsl' 2>/dev/null |
            xargs -0 shasum 2>/dev/null
    } | shasum | cut -d' ' -f1
}

CURRENT="$(fingerprint)"

if [[ -f "${MARKER}" ]] && [[ "$(cat "${MARKER}" 2>/dev/null)" == "${CURRENT}" ]]; then
    exit 0
fi

OUTPUT="$(./tools/precheck.sh "${PRESET}" 2>&1)"
STATUS=$?

if [[ ${STATUS} -eq 0 ]]; then
    mkdir -p "$(dirname "${MARKER}")"
    printf '%s' "${CURRENT}" > "${MARKER}"
    exit 0
fi

# Blocked. The whole output goes back, not a summary: the failing assertion or compiler
# diagnostic is the useful part and a summary would drop it.
{
    echo "Blocked by .claude/hooks/precheck_on_stop.sh: precheck failed on preset ${PRESET}."
    echo "CLAUDE.md's completion checklist requires it to be clean before a slice is reported"
    echo "complete. Fix the failure below, or say explicitly why the tree is being left broken."
    echo ""
    echo "${OUTPUT}"
} >&2
exit 2

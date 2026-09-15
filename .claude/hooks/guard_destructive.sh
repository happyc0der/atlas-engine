#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PreToolUse hook: refuse shell commands that destroy work without asking.
#
# CLAUDE.md forbids rewriting history and destructive git operations. A rule that depends on
# remembering it is not a rule, so this blocks the commands outright. Exit code 2 tells
# Claude Code the call was denied and why.
#
# This is a guard rail, not a security boundary: it matches command text, and anyone who
# wants to work around it trivially can. It exists to stop an accident, not an adversary.
set -uo pipefail

PAYLOAD="$(cat)"

COMMAND="$(printf '%s' "${PAYLOAD}" | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
print(data.get("tool_input", {}).get("command", ""))
' 2>/dev/null)"

[[ -z "${COMMAND}" ]] && exit 0

deny() {
    echo "Blocked by .claude/hooks/guard_destructive.sh: $1" >&2
    echo "If this is genuinely intended, run it yourself outside the agent." >&2
    exit 2
}

# Normalise whitespace so that extra spaces do not evade the match.
NORMALISED="$(printf '%s' "${COMMAND}" | tr -s '[:space:]' ' ')"

case "${NORMALISED}" in
    *"git push"*--force*|*"git push"*" -f "*|*"git push --force"*)
        deny "force push rewrites published history" ;;
    *"git reset --hard"*)
        deny "git reset --hard discards uncommitted work" ;;
    *"git clean"*-*f*)
        deny "git clean -f deletes untracked files" ;;
    *"git rebase"*)
        deny "rebase rewrites history; CLAUDE.md forbids it" ;;
    *"git filter-branch"*|*"git filter-repo"*)
        deny "history rewriting" ;;
    *"git checkout ."*|*"git restore ."*)
        deny "discards every uncommitted change in the tree" ;;
    *"rm -rf /"*|*"rm -rf ~"*|*"rm -fr /"*)
        deny "recursive delete of a root or home path" ;;
    *"rm -rf external"*|*"rm -rf .git"*)
        deny "deletes the vcpkg submodule or the repository metadata" ;;
esac

exit 0

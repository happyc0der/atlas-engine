#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run the GPU tests on the Windows machine with a GPU, from the development machine (ADR-0025 D4).
#
#   tools/ci/windows_gpu.sh [--preset windows-msvc-debug|windows-msvc-release] [--bench N] [commit]
#
# Hosted Windows runners have no GPU, so CI never runs the `gpu` label on Windows; this does, on
# a real machine over SSH. It checks the commit out there (default HEAD, which must be pushed: the
# machine fetches it from GitHub rather than being sent a working tree nobody committed), runs
# tools/ci/windows_gpu.ps1, and copies the logs and the screenshots back to
# build/windows-gpu/<commit>-<preset>/, the screenshots as PNG. Exit status is the script's. --bench N also
# runs the GPU benchmark groups N times in the desktop session, Release presets only, and copies
# their JSON back.
#
# The machine is `msi` unless ATLAS_WINDOWS_HOST names another, and the checkout is
# C:\src\atlas-engine unless ATLAS_WINDOWS_REPO does. Its desktop must be logged in and unlocked:
# the tests run there, for the reason the PowerShell script gives.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST="${ATLAS_WINDOWS_HOST:-msi}"
REMOTE_REPO="${ATLAS_WINDOWS_REPO:-C:\\src\\atlas-engine}"
REMOTE_OUT='C:\src\atlas-gpu-out'
PRESET="windows-msvc-debug"
REF="HEAD"
BENCH=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2 ;;
        --bench) BENCH="$2"; shift 2 ;;
        -h|--help) sed -n '3,18p' "$0"; exit 0 ;;
        *) REF="$1"; shift ;;
    esac
done

COMMIT="$(git -C "${REPO_ROOT}" rev-parse "${REF}")"
if [[ -z "$(git -C "${REPO_ROOT}" branch -r --contains "${COMMIT}" 2>/dev/null)" ]]; then
    echo "error: ${COMMIT:0:12} is not on any remote branch; push it first" >&2
    exit 2
fi

echo "windows gpu: ${COMMIT:0:12} on ${HOST}, ${PRESET}"
# Each step stops the run if it fails, and HEAD is then checked against the commit asked for.
# Joined by semicolons alone, a refused checkout once left the previous commit in place, and the
# run built, tested and passed that instead.
if ! ssh -o BatchMode=yes -o ConnectTimeout=15 "${HOST}" \
    "git -C '${REMOTE_REPO}' fetch --quiet origin; if (\$LASTEXITCODE) { exit 1 }; git -C '${REMOTE_REPO}' checkout --quiet --detach ${COMMIT}; if (\$LASTEXITCODE) { exit 1 }; git -C '${REMOTE_REPO}' submodule update --quiet; if (\$LASTEXITCODE) { exit 1 }; if ((git -C '${REMOTE_REPO}' rev-parse HEAD) -ne '${COMMIT}') { exit 1 }"; then
    echo "error: could not check ${COMMIT:0:12} out on ${HOST}; see above" >&2
    exit 2
fi

status=0
ssh -o BatchMode=yes -o ServerAliveInterval=30 "${HOST}" \
    "powershell -NoProfile -ExecutionPolicy Bypass -File '${REMOTE_REPO}\\tools\\ci\\windows_gpu.ps1' -Preset ${PRESET} -BenchRuns ${BENCH}" \
    | tr -d '\r' || status=$?

LOCAL_OUT="${REPO_ROOT}/build/windows-gpu/${COMMIT:0:12}-${PRESET}"
rm -rf "${LOCAL_OUT}"
mkdir -p "${LOCAL_OUT}"
scp -q "${HOST}:${REMOTE_OUT//\\//}/*" "${LOCAL_OUT}/" 2>/dev/null || true
# The copies arrive with modes made from Windows ACLs; give them ordinary ones.
chmod -R u=rwX,go=rX "${LOCAL_OUT}"

# PPM to PNG with the standard library, so nothing needs installing to look at a frame.
python3 - "${LOCAL_OUT}" <<'EOF'
import pathlib, struct, sys, zlib

def png(width, height, rgb):
    rows = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))

def ppm(data):
    """Width, height and pixels of a binary PPM. The pixels begin one byte after the last header
    field, and are not split on whitespace: a pixel byte may be one."""
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        start = at
        while not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    width, height = int(fields[1]), int(fields[2])
    return fields[0], width, height, data[at + 1:at + 1 + width * height * 3]

for path in sorted(pathlib.Path(sys.argv[1]).glob("*.ppm")):
    magic, width, height, pixels = ppm(path.read_bytes())
    if magic != b"P6":
        continue
    path.with_suffix(".png").write_bytes(png(width, height, pixels))
    print(f"  {path.with_suffix('.png')}")
EOF
exit "${status}"

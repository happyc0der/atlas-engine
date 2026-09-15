#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run the GPU-labelled tests on a software rasteriser.
#
#   tools/ci/linux_gpu.sh [preset]     default preset: linux-clang-debug
#
# The renderer is otherwise verified on exactly one graphics processor, on one developer's
# machine. That is not a position to ship from: a change that happens to work on Metal and
# breaks on Vulkan has nothing to catch it. This gives the Vulkan backend automatic coverage
# on a machine with no graphics hardware at all.
#
# Two pieces are needed and neither is obvious.
#
# **A software Vulkan implementation.** Mesa's llvmpipe presents itself as an ordinary Vulkan
# device backed by the processor. SDL_GPU's Vulkan backend enumerates it like any other, so
# nothing in Atlas needs to know.
#
# **A display.** The tests create a real window, because a swapchain needs a surface and a
# surface needs something to attach to. SDL's dummy video driver cannot provide one; it has
# no window-system integration for Vulkan to build a surface from. A virtual X server can,
# and is what turns "no monitor" into "a monitor nobody is looking at".
#
# What this does not do is replace real-hardware testing. llvmpipe is a correct Vulkan
# implementation and not a representative one: it will not reproduce a driver bug, a
# performance cliff, or anything about Metal. It catches the large class of mistakes that are
# wrong everywhere.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${1:-linux-clang-debug}"
BUILD_DIR="build/${PRESET}"
DISPLAY_NUMBER="${ATLAS_XVFB_DISPLAY:-99}"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "error: ${BUILD_DIR} does not exist; build the preset first" >&2
    exit 1
fi

for tool in Xvfb; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "error: ${tool} is not installed." >&2
        echo "  apt-get install -y xvfb mesa-vulkan-drivers libvulkan1" >&2
        exit 1
    fi
done

Xvfb ":${DISPLAY_NUMBER}" -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!

cleanup() {
    kill "${XVFB_PID}" 2>/dev/null || true
    wait "${XVFB_PID}" 2>/dev/null || true
}
trap cleanup EXIT

# Waited for rather than slept past: a fixed sleep is either too short on a loaded machine or
# wasted on an idle one.
for _ in $(seq 1 50); do
    if xdpyinfo -display ":${DISPLAY_NUMBER}" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

export DISPLAY=":${DISPLAY_NUMBER}"
export SDL_VIDEODRIVER=x11

echo "=== what Vulkan sees ==="
if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary 2>/dev/null | grep -E "deviceName|driverName" | head -4 || true
else
    echo "(vulkaninfo not installed; the tests will report whether a device appeared)"
fi

echo ""
echo "=== gpu-labelled tests (${PRESET}) ==="
# Deliberately not tolerant of an empty selection. If the label matches nothing, the tests
# were not built, and reporting success for having run none of them is the failure mode this
# whole job exists to avoid.
ctest --test-dir "${BUILD_DIR}" -L gpu --output-on-failure --no-tests=error

echo ""
echo "=== a real frame, drawn and read back ==="
# The tests cover the pieces; this covers the whole path at once, including presentation and
# the offscreen capture that a swapchain image cannot provide.
"${BUILD_DIR}/bin/atlas_sandbox" --frames 30 --grid 20 --screenshot /tmp/atlas-lavapipe.ppm

if [[ ! -s /tmp/atlas-lavapipe.ppm ]]; then
    echo "error: the sandbox reported success but wrote no screenshot" >&2
    exit 1
fi
echo "screenshot: $(wc -c < /tmp/atlas-lavapipe.ppm) bytes"

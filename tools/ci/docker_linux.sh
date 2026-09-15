#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run a Linux build of Atlas in a container, from the macOS development machine.
#
#   tools/ci/docker_linux.sh [preset]
#
# Two modes, because they verify different things:
#
#   (default) native architecture. On Apple Silicon this is an arm64 container, which
#             verifies that the code and the Linux toolchain work. It does NOT verify
#             x86_64; that is CI's job, and this script says so rather than implying parity.
#
#   --amd64   forces linux/amd64. On Apple Silicon that is emulated, which is slow and, in
#             practice, unreliable for heavy compiles: GCC has been observed to die with an
#             internal compiler error while building dependencies under emulation. Use it
#             when a specific x86_64 question needs answering locally, not routinely.
#
# Everything vcpkg keeps is architecture-specific: the vcpkg binary itself, its downloaded
# tools, and its build trees. All of that lives inside the repository, which is
# bind-mounted, so a container that used it in place would overwrite the host's macOS vcpkg
# binary with a Linux one and leave the host unable to configure. (Observed, not theorised.)
# The container therefore copies the pinned vcpkg checkout to a container-local path and
# bootstraps that, leaving the bind-mounted copy untouched.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

PLATFORM=""
TRIPLET=""
if [[ "${1:-}" == "--amd64" ]]; then
    PLATFORM="linux/amd64"
    shift
else
    case "$(uname -m)" in
        arm64|aarch64) PLATFORM="linux/arm64"; TRIPLET="arm64-linux" ;;
        *)             PLATFORM="linux/amd64" ;;
    esac
fi

PRESET="${1:-linux-clang-debug}"
IMAGE="${ATLAS_LINUX_IMAGE:-ubuntu:24.04}"

echo "Linux container check: ${IMAGE} (${PLATFORM}), preset ${PRESET}"
if [[ "${PLATFORM}" == "linux/arm64" ]]; then
    echo "note: arm64 container. Verifies the toolchain and the code, not x86_64."
fi

docker run --rm --platform "${PLATFORM}" \
    -v "${REPO_ROOT}":/src \
    -w /src \
    -e VCPKG_DOWNLOADS=/tmp/vcpkg-downloads \
    -e VCPKG_DEFAULT_BINARY_CACHE=/tmp/vcpkg-archives \
    -e ATLAS_VCPKG_TRIPLET="${TRIPLET}" \
    "${IMAGE}" \
    bash -c '
        set -euo pipefail
        mkdir -p "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}"
        bash tools/ci/linux_deps.sh > /tmp/deps.log 2>&1 || { tail -20 /tmp/deps.log; exit 1; }
        git config --global --add safe.directory /src

        # Container-local vcpkg, so the host'"'"'s binary and tool cache are never touched.
        # Cloned rather than copied: a submodule'"'"'s .git is a file pointing at the
        # superproject, and that link does not survive being moved elsewhere.
        VCPKG_PIN="$(git -C /src/external/vcpkg rev-parse HEAD)"
        git clone --quiet --no-checkout /src/external/vcpkg /opt/vcpkg
        git -C /opt/vcpkg checkout --quiet "${VCPKG_PIN}"
        /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics > /tmp/bootstrap.log 2>&1 \
            || { tail -20 /tmp/bootstrap.log; exit 1; }

        echo "arch=$(uname -m)  $(clang --version | head -1)  $(cmake --version | head -1)"
        export ATLAS_VCPKG_TOOLCHAIN=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake
        bash tools/ci/linux_build.sh '"${PRESET}"'
    '

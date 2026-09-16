#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run the software-rasteriser GPU check in a container, from the macOS development machine.
#
#   tools/ci/docker_gpu.sh [preset]     default preset: linux-clang-debug
#
# Configures the preset if it has never been configured, always builds so that whatever is
# tested is the tree as it stands rather than whatever binary an earlier run left behind, then
# runs tools/ci/linux_gpu.sh inside the same container. Continuous integration runs the
# identical script, so a failure here is a failure there and the two cannot drift.
#
# The build is unconditional because the alternative was found the hard way: a check keyed on
# the build directory existing reported a green GPU lane against a binary from before the
# change under test. An up-to-date build costs a few seconds; a stale one costs the truth.
#
# The architecture caveat from docker_linux.sh applies: on Apple Silicon this is an arm64
# container, so it verifies the Vulkan backend and the code, not x86_64.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${1:-linux-clang-debug}"
IMAGE="${ATLAS_LINUX_IMAGE:-ubuntu:24.04}"

case "$(uname -m)" in
    arm64|aarch64) PLATFORM="linux/arm64"; TRIPLET="arm64-linux" ;;
    *)             PLATFORM="linux/amd64"; TRIPLET="" ;;
esac

echo "Software-rasteriser GPU check: ${IMAGE} (${PLATFORM}), preset ${PRESET}"

docker run --rm --platform "${PLATFORM}" \
    -v "${REPO_ROOT}":/src \
    -w /src \
    -e VCPKG_DOWNLOADS=/tmp/vcpkg-downloads \
    -e VCPKG_DEFAULT_BINARY_CACHE=/tmp/vcpkg-archives \
    -e ATLAS_VCPKG_TRIPLET="${TRIPLET}" \
    -e ATLAS_PRESET="${PRESET}" \
    "${IMAGE}" \
    bash -c '
        set -euo pipefail
        mkdir -p "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}"
        bash tools/ci/linux_deps.sh > /tmp/deps.log 2>&1 || { tail -20 /tmp/deps.log; exit 1; }
        bash tools/ci/linux_gpu_deps.sh > /tmp/gpu-deps.log 2>&1 \
            || { tail -20 /tmp/gpu-deps.log; exit 1; }
        git config --global --add safe.directory /src

        # The container is discarded after each run, so the toolchain the cached configuration
        # points at must be recreated every time, whether or not a build directory exists.
        VCPKG_PIN="$(git -C /src/external/vcpkg rev-parse HEAD)"
        git clone --quiet --no-checkout /src/external/vcpkg /opt/vcpkg
        git -C /opt/vcpkg checkout --quiet "${VCPKG_PIN}"
        /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics > /tmp/bootstrap.log 2>&1 \
            || { tail -20 /tmp/bootstrap.log; exit 1; }
        export ATLAS_VCPKG_TOOLCHAIN=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake
        export CC=clang-19 CXX=clang++-19

        if [[ ! -d "build/${ATLAS_PRESET}/bin" ]]; then
            cmake --preset "${ATLAS_PRESET}"
        else
            echo "build directory exists for ${ATLAS_PRESET}; rebuilding whatever changed"
        fi
        cmake --build --preset "${ATLAS_PRESET}"

        bash tools/ci/linux_gpu.sh "${ATLAS_PRESET}"
    '

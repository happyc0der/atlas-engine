#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Configure, build, and test Atlas on Linux.
#
#   tools/ci/linux_build.sh [preset]
#
# Used both by CI and by the local Docker container that gives Linux toolchain parity on
# the development machine. Note that the local container is arm64: it verifies the
# toolchain and the code, not the x86_64 architecture. CI covers x86_64.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${1:-linux-clang-debug}"

# The presets pin x64-linux, which is what CI builds. A local container on Apple Silicon
# runs arm64, so the triplet can be overridden for that case. Overriding it verifies the
# toolchain and the code; it does not verify the x86_64 architecture, which stays CI's job.
CONFIGURE_ARGS=()
if [[ -n "${ATLAS_VCPKG_TRIPLET:-}" ]]; then
    CONFIGURE_ARGS+=("-DVCPKG_TARGET_TRIPLET=${ATLAS_VCPKG_TRIPLET}")
    echo "note: overriding vcpkg triplet to ${ATLAS_VCPKG_TRIPLET} (architecture parity not verified)"
fi

# A container run supplies its own vcpkg so that it never writes to the bind-mounted one.
if [[ -n "${ATLAS_VCPKG_TOOLCHAIN:-}" ]]; then
    CONFIGURE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${ATLAS_VCPKG_TOOLCHAIN}")
elif [[ ! -x external/vcpkg/vcpkg ]]; then
    echo "bootstrapping vcpkg"
    ./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
fi

echo "=== configure (${PRESET}) ==="
cmake --preset "${PRESET}" "${CONFIGURE_ARGS[@]}"

echo "=== build (${PRESET}) ==="
cmake --build --preset "${PRESET}"

echo "=== test (${PRESET}) ==="
ctest --preset "${PRESET}"

echo "=== run (${PRESET}) ==="
"./build/${PRESET}/bin/atlas_sandbox" --version
"./build/${PRESET}/bin/atlas_sandbox" --headless --ticks 120

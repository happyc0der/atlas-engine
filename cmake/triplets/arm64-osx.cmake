# SPDX-License-Identifier: GPL-3.0-or-later
# Overlay triplet: arm64-osx.
#
# Identical to the upstream triplet except that the deployment target is pinned to match
# the project's CMAKE_OSX_DEPLOYMENT_TARGET. A mismatch between dependency binaries and
# first-party code produces linker warnings and, occasionally, real failures.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 15.0)

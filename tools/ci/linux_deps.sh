#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# System packages a Linux build of Atlas needs, beyond the compiler.
#
# vcpkg builds dependencies from source, so their own build-time prerequisites have to be
# present. SDL3 (from M1) additionally needs the display and audio development headers even
# for a headless build, because the library still compiles those backends.
set -euo pipefail

SUDO=""
if [[ "${EUID}" -ne 0 ]]; then
    SUDO="sudo"
fi

export DEBIAN_FRONTEND=noninteractive

${SUDO} apt-get update -qq

# clang-19 and libstdc++-14 specifically, not whatever "clang" happens to be.
# Clang 18 reports __cpp_concepts=201907, and libstdc++ gates <expected> on >=202002L, so
# clang 18 can never see std::expected no matter which libstdc++ is installed. Clang 19
# reports 202002 and works. libstdc++ rather than libc++ keeps the standard-library ABI
# consistent with the dependencies vcpkg builds with GCC.
# autoconf, automake, libtool and autoconf-archive are vcpkg's prerequisites, not Atlas's.
# Some ports build with autotools and vcpkg runs autoreconf on them; libxcrypt, which arrives
# transitively on x64-linux, is one. Without these the failure is "building libxcrypt failed"
# during configure, several layers below anything Atlas wrote. They are not needed on every
# platform or every triplet, which is why this was invisible until x86_64 continuous
# integration ran for the first time. libltdl-dev is the same story one layer down: libxcrypt
# links against ltdl and vcpkg checks for its development headers before it will even
# configure.
${SUDO} apt-get install -y --no-install-recommends \
    autoconf \
    autoconf-archive \
    automake \
    libtool \
    libltdl-dev \
    build-essential \
    ca-certificates \
    clang-19 \
    g++-14 \
    libstdc++-14-dev \
    cmake \
    curl \
    git \
    ninja-build \
    pkg-config \
    python3 \
    tar \
    unzip \
    zip

# SDL3 build prerequisites. vcpkg builds SDL3 from source, and the port enables the X11,
# Wayland, D-Bus and ibus backends on Linux, so their development headers must be present
# even for a build that will only ever run headless.
#
# Not tolerant of failure: a missing package here surfaces much later as an opaque
# "SDL_missing_dependency" from inside SDL's own configure, which is a miserable thing to
# diagnose. libxtst-dev is the one that is easy to forget and was, once.
${SUDO} apt-get install -y --no-install-recommends \
    libasound2-dev \
    libdbus-1-dev \
    libegl1-mesa-dev \
    libgl1-mesa-dev \
    libibus-1.0-dev \
    libpulse-dev \
    libudev-dev \
    libwayland-dev \
    libx11-dev \
    libxcursor-dev \
    libxext-dev \
    libxfixes-dev \
    libxi-dev \
    libxkbcommon-dev \
    libxrandr-dev \
    libxss-dev \
    libxtst-dev \
    wayland-protocols

echo "Linux prerequisites installed"

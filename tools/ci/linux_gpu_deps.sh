#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The extra system packages the software-rasteriser GPU check needs, on top of
# tools/ci/linux_deps.sh.
#
# Kept separate because an ordinary headless build needs none of them, and installing a
# Vulkan stack and an X server on every build would be minutes of nothing useful.
set -euo pipefail

SUDO=""
if [[ "${EUID}" -ne 0 ]]; then
    SUDO="sudo"
fi

export DEBIAN_FRONTEND=noninteractive
${SUDO} apt-get update -qq

# mesa-vulkan-drivers supplies llvmpipe, the software Vulkan device. libvulkan1 is the loader
# that finds it. vulkan-tools is only for the diagnostic line the script prints, and is worth
# the download the first time a job fails to see a device.
#
# Xvfb is the virtual X server. The window-system libraries are SDL's runtime dependencies:
# it dynamically loads what it needs, so a missing one shows up as "no video driver" rather
# than as a link error, which is a confusing way to learn about it.
${SUDO} apt-get install -y --no-install-recommends \
    mesa-vulkan-drivers \
    libvulkan1 \
    vulkan-tools \
    xvfb \
    x11-utils \
    libx11-6 \
    libxext6 \
    libxrandr2 \
    libxi6 \
    libxcursor1 \
    libxfixes3 \
    libxss1 \
    libxkbcommon0 \
    libwayland-client0 \
    libdecor-0-0 \
    libasound2t64 \
    libpulse0 \
    libudev1 \
    libdbus-1-3

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Writing a captured frame to a Portable Pixmap.
///
/// PPM because it needs no library and every image viewer reads it. The capture is whatever
/// the swapchain format is, usually blue-green-red-alpha rather than the red-green-blue PPM
/// wants, so the channels are reordered on the way out. That reordering is the one thing in
/// here that can be silently wrong, which is why it has a test.
///
/// Thread affinity: any; it touches only its arguments and the filesystem.

#include <atlas/core/result.hpp>
#include <atlas/rhi/device.hpp>

#include <filesystem>

namespace atlas::app {

/// Failure: InvalidArgument for an empty capture, PermissionDenied if the file cannot be
/// opened, IoFailure if the write fails part way.
[[nodiscard]] Status write_ppm(const std::filesystem::path& path,
                               const rhi::Device::Capture& capture);

}  // namespace atlas::app

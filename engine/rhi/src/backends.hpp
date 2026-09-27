// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What the device asks the graphics library for, and which backends this build of it has.
///
/// **Why this exists** (ADR-0025). Until M28 the device asked SDL for DXIL as well as the two
/// formats Atlas ships, and SDL on Windows was built with Direct3D 12 as its only GPU backend.
/// So Windows got a backend no Atlas shader could load on, and nothing noticed, because no lane
/// with a GPU runs Windows. The device's request comes from here, so a test with no device can
/// check both halves: that the request names only formats Atlas ships, and that this build of
/// the library has at least one backend taking one of them.
///
/// Private to the module, and free of the library's types so a test can include it.
/// Thread affinity: none; these read compiled-in facts and allocate.

#include <atlas/rhi/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace atlas::rhi::detail {

/// The formats `Device::create` asks the library for: `kShippedShaderFormats`, and nothing else.
[[nodiscard]] std::vector<ShaderFormat> requested_shader_formats();

/// The GPU backends this build of the library was compiled with, by the library's own names
/// ("metal", "direct3d12", "vulkan"). Compiled in, not probed: the list is the same on a
/// machine with no graphics device, which is what lets a test read it anywhere.
[[nodiscard]] std::vector<std::string> compiled_backends();

/// The formats a backend of that name takes, among those Atlas has a name for. Empty for a
/// name Atlas does not know, so a new backend in the library is reported rather than assumed.
[[nodiscard]] std::vector<ShaderFormat> backend_shader_formats(std::string_view backend);

}  // namespace atlas::rhi::detail

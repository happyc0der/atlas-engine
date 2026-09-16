// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Loading cooked shaders from disk.
///
/// Reads cooked shaders by path rather than through the asset registry. That is a recorded
/// deferral, not an oversight: the registry has nothing to add to a shader whose resource
/// counts are already compile-time constants from the generated manifest. See
/// docs/DEFERRED.md under M4. It moves behind the registry when the registry can offer it
/// something, such as hot reload of a re-cooked shader.

#include <atlas/core/result.hpp>
#include <atlas/rhi/device.hpp>

#include <string_view>

namespace atlas::renderer {

struct ShaderPair {
    rhi::ShaderHandle vertex;
    rhi::ShaderHandle fragment;
};

/// Load `<name>.vert` and `<name>.frag` in whichever cooked format the device accepts.
///
/// On failure, any shader already created is destroyed, so a partial load leaves nothing
/// behind.
[[nodiscard]] Result<ShaderPair> load_shader_pair(rhi::Device& device, std::string_view directory,
                                                  std::string_view name);

}  // namespace atlas::renderer

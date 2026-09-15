// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Loading cooked shaders from disk.
///
/// A stopgap, and labelled as one: it reads files by path, which the asset system in M4
/// replaces with virtual paths, identifiers and asynchronous loading. What it does do now is
/// pick the format the device accepts and feed the reflected resource counts into shader
/// creation, so the counts cannot disagree with the shaders.

#include <atlas/core/result.hpp>
#include <atlas/rhi/device.hpp>

#include <string_view>

namespace atlas::renderer::detail {

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

}  // namespace atlas::renderer::detail

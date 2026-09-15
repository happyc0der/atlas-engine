// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Handles to graphics resources.
///
/// Every resource the device owns is referred to by a generation-counted handle rather than
/// a pointer, so destroying a resource makes every outstanding reference to it stop
/// resolving instead of dangling. The tags keep the handle types apart: a shader handle
/// cannot be passed where a buffer handle is expected.
///
#include <atlas/core/handle.hpp>

namespace atlas::rhi {

struct BufferTag;
struct TextureTag;
struct SamplerTag;
struct ShaderTag;
struct GraphicsPipelineTag;

using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using ShaderHandle = Handle<ShaderTag>;
using GraphicsPipelineHandle = Handle<GraphicsPipelineTag>;

}  // namespace atlas::rhi

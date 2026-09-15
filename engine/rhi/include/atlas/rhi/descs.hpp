// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Descriptors: what to create, separated from the creating.
///
/// Every descriptor carries a debug_name. It costs nothing in release, it labels the object
/// in a graphics debugger, and it is what makes the leak report at device shutdown say
/// which resource leaked rather than which slot index did.

#include <atlas/rhi/handles.hpp>
#include <atlas/rhi/types.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace atlas::rhi {

struct DeviceDesc {
    /// Enable the graphics API's validation layers. On by default in debug builds; they
    /// cost performance and are worth it while the renderer is being written.
    bool debug = false;

    /// Ask for a particular backend. Unknown lets the system choose, which is almost always
    /// right; naming one is for reproducing a backend-specific problem.
    Backend preferred_backend = Backend::Unknown;

    std::string_view debug_name = "atlas device";
};

struct BufferDesc {
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    std::string_view debug_name = "buffer";
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    ShaderFormat format = ShaderFormat::SpirV;

    /// Compiled shader code. Copied during creation, so the caller may release it after.
    std::span<const std::byte> code;

    /// Entry point name. Cross-compiled Metal shaders conventionally use "main0" rather
    /// than "main", which is the kind of detail that costs an afternoon when undocumented.
    std::string_view entry_point = "main";

    std::string_view debug_name = "shader";
};

/// How vertex data is laid out for a pipeline.
struct VertexLayout {
    /// Bytes between consecutive vertices. Zero means the pipeline takes no vertex buffer,
    /// which is how the first triangle is drawn: from the vertex index alone.
    std::uint32_t stride = 0;
    std::span<const VertexAttribute> attributes;
};

struct GraphicsPipelineDesc {
    ShaderHandle vertex_shader;
    ShaderHandle fragment_shader;
    VertexLayout vertex_layout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;

    /// Format of the colour target this pipeline will draw into. Must match the target, so
    /// it usually comes from the swapchain.
    TextureFormat colour_format = TextureFormat::Unknown;

    std::string_view debug_name = "pipeline";
};

/// What a render pass does with its colour target when it begins and ends.
enum class LoadOp : std::uint8_t {
    /// Replace the contents with the clear colour. Cheapest, and correct whenever the pass
    /// draws every pixel it cares about.
    Clear,
    /// Keep what is already there.
    Load,
    /// Do not care. Lets a tiled renderer skip reading the previous contents.
    DontCare,
};

struct ColourTargetDesc {
    LoadOp load = LoadOp::Clear;
    Colour clear_colour;
};

struct RenderPassDesc {
    ColourTargetDesc colour;
    std::string_view debug_name = "render pass";
};

}  // namespace atlas::rhi

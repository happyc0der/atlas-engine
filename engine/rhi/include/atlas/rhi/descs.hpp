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

    /// When finished frames reach the display. A request the system may refuse, in which
    /// case the device falls back to waiting for the refresh and says so.
    PresentMode present_mode = PresentMode::Vsync;

    std::string_view debug_name = "atlas device";
};

struct BufferDesc {
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    std::string_view debug_name = "buffer";
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Rgba8Unorm;
    std::string_view debug_name = "texture";
};

struct SamplerDesc {
    Filter min_filter = Filter::Linear;
    Filter mag_filter = Filter::Linear;
    AddressMode address_u = AddressMode::ClampToEdge;
    AddressMode address_v = AddressMode::ClampToEdge;
    std::string_view debug_name = "sampler";
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    ShaderFormat format = ShaderFormat::SpirV;

    /// Compiled shader code. Copied during creation, so the caller may release it after.
    std::span<const std::byte> code;

    /// Entry point name. Cross-compiled Metal shaders conventionally use "main0" rather
    /// than "main", which is the kind of detail that costs an afternoon when undocumented.
    std::string_view entry_point = "main";

    /// Resources the shader declares. These must match the compiled shader exactly: a
    /// disagreement produces a driver-level failure with no useful message attached. They
    /// are read out of the shader by reflection in tools/cook_shaders.py and reach the
    /// caller as generated constants, so they cannot drift.
    std::uint32_t samplers = 0;  ///< Sampled textures, each paired with one sampler.
    std::uint32_t storage_textures = 0;
    std::uint32_t storage_buffers = 0;
    std::uint32_t uniform_buffers = 0;

    std::string_view debug_name = "shader";
};

/// One stream of vertex data.
///
/// Two streams cover the batching case: slot zero holds the four corners of a unit quad,
/// shared by every instance, and slot one holds whatever differs per instance. Splitting
/// them is what lets thousands of quads be drawn from one small buffer plus one array.
struct VertexStream {
    /// Bytes between consecutive elements.
    std::uint32_t stride = 0;
    /// Whether the stream advances per vertex or per instance.
    bool per_instance = false;
    std::span<const VertexAttribute> attributes;
};

/// How vertex data is laid out for a pipeline.
///
/// An empty stream list means the pipeline reads no vertex buffer at all, which is how the
/// first triangle is drawn: from the vertex index alone.
struct VertexLayout {
    std::span<const VertexStream> streams;
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

/// A texture and the sampler it is read through, bound together.
///
/// One structure rather than two parallel arrays because SDL binds them in pairs, and
/// because a texture bound without its sampler is not a state worth being able to express.
struct TextureSamplerBinding {
    TextureHandle texture;
    SamplerHandle sampler;
};

struct RenderPassDesc {
    ColourTargetDesc colour;
    std::string_view debug_name = "render pass";
};

}  // namespace atlas::rhi

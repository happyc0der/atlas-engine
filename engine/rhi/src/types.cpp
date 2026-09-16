// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/rhi/descs.hpp>
#include <atlas/rhi/types.hpp>

namespace atlas::rhi {

std::string_view to_string(Backend backend) noexcept {
    switch (backend) {
    case Backend::Unknown: return "unknown";
    case Backend::Metal: return "metal";
    case Backend::Vulkan: return "vulkan";
    case Backend::Direct3D12: return "direct3d12";
    }
    return "unrecognised";
}

std::string_view to_string(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::SpirV: return "spirv";
    case ShaderFormat::Msl: return "msl";
    case ShaderFormat::Dxil: return "dxil";
    }
    return "unrecognised";
}

std::string_view to_string(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::Fragment: return "fragment";
    }
    return "unrecognised";
}

std::string_view to_string(BufferUsage usage) noexcept {
    switch (usage) {
    case BufferUsage::Vertex: return "vertex";
    case BufferUsage::Index: return "index";
    }
    return "unrecognised";
}

std::string_view to_string(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Unknown: return "unknown";
    case TextureFormat::Bgra8Unorm: return "bgra8unorm";
    case TextureFormat::Rgba8Unorm: return "rgba8unorm";
    case TextureFormat::Bgra8UnormSrgb: return "bgra8unorm-srgb";
    case TextureFormat::Rgba8UnormSrgb: return "rgba8unorm-srgb";
    case TextureFormat::R32Uint: return "r32uint";
    }
    return "unrecognised";
}

std::string_view to_string(PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::Vsync: return "vsync";
    case PresentMode::Immediate: return "immediate";
    }
    return "unrecognised";
}

std::string_view to_string(Filter filter) noexcept {
    switch (filter) {
    case Filter::Nearest: return "nearest";
    case Filter::Linear: return "linear";
    }
    return "unrecognised";
}

std::string_view to_string(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::ClampToEdge: return "clamp-to-edge";
    case AddressMode::Repeat: return "repeat";
    case AddressMode::MirroredRepeat: return "mirrored-repeat";
    }
    return "unrecognised";
}

std::uint32_t byte_size(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1: return 4;
    case VertexFormat::Float2: return 8;
    case VertexFormat::Float3: return 12;
    case VertexFormat::Float4: return 16;
    case VertexFormat::UByte4Norm: return 4;
    }
    return 0;
}

std::uint32_t byte_size(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Unknown: return 0;
    case TextureFormat::Bgra8Unorm:
    case TextureFormat::Rgba8Unorm:
    case TextureFormat::Bgra8UnormSrgb:
    case TextureFormat::Rgba8UnormSrgb:
    case TextureFormat::R32Uint: return 4;
    }
    return 0;
}

std::string_view to_string(BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::AlphaBlend: return "alpha-blend";
    case BlendMode::Replace: return "replace";
    }
    return "unrecognised";
}

std::string to_string(TextureUsage usage) {
    if (usage.sampled && usage.colour_target) {
        return "sampled|colour-target";
    }
    if (usage.sampled) {
        return "sampled";
    }
    if (usage.colour_target) {
        return "colour-target";
    }
    return "none";
}

}  // namespace atlas::rhi

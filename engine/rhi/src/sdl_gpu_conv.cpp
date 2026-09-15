// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_gpu_conv.hpp"

#include <atlas/rhi/descs.hpp>

#include <string_view>

namespace atlas::rhi::detail {

Backend backend_from_driver(const char* driver_name) noexcept {
    if (driver_name == nullptr) {
        return Backend::Unknown;
    }
    const std::string_view name{driver_name};
    if (name == "metal") {
        return Backend::Metal;
    }
    if (name == "vulkan") {
        return Backend::Vulkan;
    }
    if (name == "direct3d12") {
        return Backend::Direct3D12;
    }
    return Backend::Unknown;
}

SDL_GPUShaderFormat to_sdl(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::SpirV: return SDL_GPU_SHADERFORMAT_SPIRV;
    case ShaderFormat::Msl: return SDL_GPU_SHADERFORMAT_MSL;
    case ShaderFormat::Dxil: return SDL_GPU_SHADERFORMAT_DXIL;
    }
    return SDL_GPU_SHADERFORMAT_INVALID;
}

SDL_GPUShaderStage to_sdl(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex: return SDL_GPU_SHADERSTAGE_VERTEX;
    case ShaderStage::Fragment: return SDL_GPU_SHADERSTAGE_FRAGMENT;
    }
    return SDL_GPU_SHADERSTAGE_VERTEX;
}

SDL_GPUBufferUsageFlags to_sdl(BufferUsage usage) noexcept {
    switch (usage) {
    case BufferUsage::Vertex: return SDL_GPU_BUFFERUSAGE_VERTEX;
    case BufferUsage::Index: return SDL_GPU_BUFFERUSAGE_INDEX;
    }
    return SDL_GPU_BUFFERUSAGE_VERTEX;
}

SDL_GPUPrimitiveType to_sdl(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleList: return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip: return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    case PrimitiveTopology::LineList: return SDL_GPU_PRIMITIVETYPE_LINELIST;
    case PrimitiveTopology::PointList: return SDL_GPU_PRIMITIVETYPE_POINTLIST;
    }
    return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

SDL_GPUVertexElementFormat to_sdl(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    case VertexFormat::Float2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case VertexFormat::Float3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    case VertexFormat::Float4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    case VertexFormat::UByte4Norm: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
}

SDL_GPULoadOp to_sdl(LoadOp op) noexcept {
    switch (op) {
    case LoadOp::Clear: return SDL_GPU_LOADOP_CLEAR;
    case LoadOp::Load: return SDL_GPU_LOADOP_LOAD;
    case LoadOp::DontCare: return SDL_GPU_LOADOP_DONT_CARE;
    }
    return SDL_GPU_LOADOP_CLEAR;
}

SDL_GPUFilter to_sdl(Filter filter) noexcept {
    switch (filter) {
    case Filter::Nearest: return SDL_GPU_FILTER_NEAREST;
    case Filter::Linear: return SDL_GPU_FILTER_LINEAR;
    }
    return SDL_GPU_FILTER_LINEAR;
}

SDL_GPUSamplerAddressMode to_sdl(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::ClampToEdge: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    case AddressMode::Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case AddressMode::MirroredRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

TextureFormat from_sdl(SDL_GPUTextureFormat format) noexcept {
    switch (format) {
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM: return TextureFormat::Bgra8Unorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM: return TextureFormat::Rgba8Unorm;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB: return TextureFormat::Bgra8UnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB: return TextureFormat::Rgba8UnormSrgb;
    default: return TextureFormat::Unknown;
    }
}

SDL_GPUTextureFormat to_sdl(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Bgra8Unorm: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case TextureFormat::Rgba8Unorm: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case TextureFormat::Bgra8UnormSrgb: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    case TextureFormat::Rgba8UnormSrgb: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::Unknown: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

}  // namespace atlas::rhi::detail

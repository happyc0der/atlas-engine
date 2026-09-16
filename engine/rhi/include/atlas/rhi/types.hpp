// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Enumerations and small values used across the render hardware interface.
///
/// These are Atlas's own vocabulary, not the graphics library's. The mapping lives in
/// engine/rhi/src, so a future native backend changes one file rather than every caller.
/// See docs/adr/0002-rendering-backend.md.

#include <cstdint>
#include <string>
#include <string_view>

namespace atlas::rhi {

/// Which graphics API is driving the device.
enum class Backend : std::uint8_t {
    Unknown,
    Metal,
    Vulkan,
    Direct3D12,
};

[[nodiscard]] std::string_view to_string(Backend backend) noexcept;

/// Compiled shader representations.
///
/// Atlas ships SPIR-V and Metal Shading Language. DXIL, and with it the Direct3D 12
/// backend, waits for a shader compiler that runs on the development machine; see
/// docs/adr/0006-shader-toolchain.md.
enum class ShaderFormat : std::uint8_t {
    SpirV,
    Msl,
    Dxil,
};

[[nodiscard]] std::string_view to_string(ShaderFormat format) noexcept;

enum class ShaderStage : std::uint8_t {
    Vertex,
    Fragment,
};

[[nodiscard]] std::string_view to_string(ShaderStage stage) noexcept;

/// What a buffer will be used for. Drives the allocation, so it cannot be changed later.
enum class BufferUsage : std::uint8_t {
    Vertex,
    Index,
};

[[nodiscard]] std::string_view to_string(BufferUsage usage) noexcept;

enum class PrimitiveTopology : std::uint8_t {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList,
};

/// Vertex attribute element types, named by what they hold rather than by a platform's
/// spelling of it.
enum class VertexFormat : std::uint8_t {
    Float1,
    Float2,
    Float3,
    Float4,
    UByte4Norm,
};

/// Size in bytes of one vertex attribute of this format.
[[nodiscard]] std::uint32_t byte_size(VertexFormat format) noexcept;

struct VertexAttribute {
    /// Matches the location in the shader's input signature.
    std::uint32_t location = 0;
    std::uint32_t offset = 0;
    VertexFormat format = VertexFormat::Float4;
};

/// A colour in linear space, with components in [0, 1].
struct Colour {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend constexpr bool operator==(Extent2D, Extent2D) noexcept = default;
};

/// What a texture holds, as far as the renderer needs to know at this milestone.
enum class TextureFormat : std::uint8_t {
    Unknown,
    Bgra8Unorm,
    Rgba8Unorm,
    Bgra8UnormSrgb,
    Rgba8UnormSrgb,
    /// One unsigned 32-bit integer per pixel.
    ///
    /// For an identifier target, where a value read back must be exactly a value that was
    /// written. No blending, no filtering, no colour-space conversion: each of those would
    /// produce identifiers nothing ever wrote.
    R32Uint,
};

[[nodiscard]] std::string_view to_string(TextureFormat format) noexcept;

/// Bytes per pixel, or zero for an unknown format.
[[nodiscard]] std::uint32_t byte_size(TextureFormat format) noexcept;

/// Whether a format holds unsigned integers rather than normalised colour.
///
/// The distinction is not cosmetic. Blending an integer target aborts the process on Metal
/// and is silently accepted on Vulkan, which would produce blended identifiers; both were
/// measured. Atlas refuses the combination itself so the behaviour is the same everywhere
/// and the message names the cause.
[[nodiscard]] constexpr bool is_integer_format(TextureFormat format) noexcept {
    return format == TextureFormat::R32Uint;
}

/// What a texture may be used for.
///
/// Drives the allocation, so it cannot be changed afterwards. Two independent answers rather
/// than a flag enumeration, because both combinations are real: an identifier target is
/// written by a pass and never sampled, and every texture before M7 was sampled and never
/// written.
struct TextureUsage {
    /// Readable by a shader through a sampler.
    bool sampled = true;
    /// Writable by a render pass.
    bool colour_target = false;

    [[nodiscard]] friend constexpr bool operator==(TextureUsage, TextureUsage) noexcept = default;
};

[[nodiscard]] std::string to_string(TextureUsage usage);

/// A rectangle of texels, in pixels, with the origin at the top-left.
struct Rect2D {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    Extent2D extent;

    [[nodiscard]] friend constexpr bool operator==(Rect2D, Rect2D) noexcept = default;
};

/// When a finished frame is handed to the display.
enum class PresentMode : std::uint8_t {
    /// Wait for the display's refresh. No tearing, and the frame rate is capped by the
    /// display, which is what an interactive application wants.
    Vsync,
    /// Present immediately. Tears, and is the only honest way to measure how long the
    /// engine itself takes: with vsync on, every measurement is the refresh interval.
    Immediate,
};

[[nodiscard]] std::string_view to_string(PresentMode mode) noexcept;

/// How a texture is sampled between texels.
enum class Filter : std::uint8_t {
    /// Nearest texel. What pixel art and an integer-ID target want: interpolating an
    /// identifier would produce identifiers that were never written.
    Nearest,
    /// Linear blend of the neighbouring texels.
    Linear,
};

/// What happens outside the zero-to-one range.
enum class AddressMode : std::uint8_t {
    ClampToEdge,
    Repeat,
    MirroredRepeat,
};

[[nodiscard]] std::string_view to_string(Filter filter) noexcept;
[[nodiscard]] std::string_view to_string(AddressMode mode) noexcept;

}  // namespace atlas::rhi

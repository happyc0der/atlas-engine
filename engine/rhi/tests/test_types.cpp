// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/rhi/handles.hpp>
#include <atlas/rhi/types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <set>
#include <string_view>
#include <type_traits>

using atlas::rhi::Backend;
using atlas::rhi::BufferHandle;
using atlas::rhi::BufferUsage;
using atlas::rhi::byte_size;
using atlas::rhi::Extent2D;
using atlas::rhi::GraphicsPipelineHandle;
using atlas::rhi::ShaderFormat;
using atlas::rhi::ShaderHandle;
using atlas::rhi::ShaderStage;
using atlas::rhi::TextureFormat;
using atlas::rhi::to_string;
using atlas::rhi::VertexFormat;

TEST_CASE("every backend, format and stage has a distinct name", "[rhi][types]") {
    // These names end up in logs and in bug reports. A missing one prints as a number and a
    // duplicate makes two different things indistinguishable.
    const auto distinct = [](auto values, auto namer) {
        std::set<std::string_view> names;
        for (const auto value : values) {
            const std::string_view name = namer(value);
            CHECK_FALSE(name.empty());
            CHECK(name != "unrecognised");
            CHECK(names.insert(name).second);
        }
    };

    distinct(std::array{Backend::Unknown, Backend::Metal, Backend::Vulkan, Backend::Direct3D12},
             [](Backend b) { return to_string(b); });
    distinct(std::array{ShaderFormat::SpirV, ShaderFormat::Msl, ShaderFormat::Dxil},
             [](ShaderFormat f) { return to_string(f); });
    distinct(std::array{ShaderStage::Vertex, ShaderStage::Fragment},
             [](ShaderStage s) { return to_string(s); });
    distinct(std::array{BufferUsage::Vertex, BufferUsage::Index},
             [](BufferUsage u) { return to_string(u); });
    distinct(std::array{TextureFormat::Unknown, TextureFormat::Bgra8Unorm,
                        TextureFormat::Rgba8Unorm, TextureFormat::Bgra8UnormSrgb,
                        TextureFormat::Rgba8UnormSrgb},
             [](TextureFormat f) { return to_string(f); });
}

TEST_CASE("vertex attribute sizes are what the shader will expect", "[rhi][types]") {
    // A wrong size here produces a pipeline that reads its inputs at the wrong offsets, and
    // the result is geometry that is subtly, silently wrong.
    CHECK(byte_size(VertexFormat::Float1) == 4);
    CHECK(byte_size(VertexFormat::Float2) == 8);
    CHECK(byte_size(VertexFormat::Float3) == 12);
    CHECK(byte_size(VertexFormat::Float4) == 16);
    CHECK(byte_size(VertexFormat::UByte4Norm) == 4);
}

TEST_CASE("texture pixel sizes are known for every supported format", "[rhi][types]") {
    CHECK(byte_size(TextureFormat::Unknown) == 0);
    CHECK(byte_size(TextureFormat::Bgra8Unorm) == 4);
    CHECK(byte_size(TextureFormat::Rgba8Unorm) == 4);
    CHECK(byte_size(TextureFormat::Bgra8UnormSrgb) == 4);
    CHECK(byte_size(TextureFormat::Rgba8UnormSrgb) == 4);
}

TEST_CASE("resource handle types are distinct", "[rhi][types]") {
    // The tags exist so that a shader handle cannot be passed where a buffer handle is
    // wanted. If these ever became the same type, that protection would be gone silently.
    STATIC_REQUIRE_FALSE(std::is_same_v<BufferHandle, ShaderHandle>);
    STATIC_REQUIRE_FALSE(std::is_same_v<ShaderHandle, GraphicsPipelineHandle>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<BufferHandle>);
}

TEST_CASE("a default handle is null", "[rhi][types]") {
    CHECK_FALSE(BufferHandle{}.valid());
    CHECK_FALSE(ShaderHandle{}.valid());
    CHECK_FALSE(GraphicsPipelineHandle{}.valid());
}

TEST_CASE("extents compare by value", "[rhi][types]") {
    CHECK(Extent2D{.width = 4, .height = 2} == Extent2D{.width = 4, .height = 2});
    CHECK_FALSE(Extent2D{.width = 4, .height = 2} == Extent2D{.width = 2, .height = 4});
}

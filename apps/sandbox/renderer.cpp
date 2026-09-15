// SPDX-License-Identifier: GPL-3.0-or-later
#include "renderer.hpp"

#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace atlas::sandbox {
namespace {

constexpr log::Category kApp = log::category::kApp;

[[nodiscard]] Result<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(
            Error(ErrorCode::NotFound,
                  std::format("cannot read '{}': {}", path.string(), error.message())));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("cannot open '{}'", path.string())));
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): file reads are bytes.
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!stream) {
            return std::unexpected(Error(ErrorCode::MalformedData,
                                         std::format("short read from '{}'", path.string())));
        }
    }
    return bytes;
}

/// Which cooked format this device takes, with the extension and entry point that go with
/// it.
struct ShaderChoice {
    rhi::ShaderFormat format = rhi::ShaderFormat::SpirV;
    std::string_view extension;
    std::string_view entry_point;
};

[[nodiscard]] Result<ShaderChoice> choose_format(const rhi::Device& device) {
    // Metal Shading Language is shipped as source and compiled by the driver; SPIR-V is
    // consumed directly. Both come out of tools/cook_shaders.py. DXIL is absent because no
    // compiler for it runs on the development machine; see ADR-0006.
    if (device.supports_shader_format(rhi::ShaderFormat::Msl)) {
        return ShaderChoice{.format = rhi::ShaderFormat::Msl,
                            .extension = ".msl",
                            // SPIRV-Cross renames the entry point; the manifest records it.
                            .entry_point = "main0"};
    }
    if (device.supports_shader_format(rhi::ShaderFormat::SpirV)) {
        return ShaderChoice{
            .format = rhi::ShaderFormat::SpirV, .extension = ".spv", .entry_point = "main"};
    }
    return std::unexpected(
        Error(ErrorCode::NotSupported,
              std::format("the {} backend accepts neither SPIR-V nor Metal Shading Language, and "
                          "Atlas ships no other format",
                          device.backend_name())));
}

}  // namespace

Result<TriangleRenderer> TriangleRenderer::create(rhi::Device& device,
                                                  std::string_view shader_directory) {
    const auto choice = choose_format(device);
    if (!choice) {
        return std::unexpected(choice.error());
    }

    const std::filesystem::path directory{shader_directory};
    const auto vertex_path = directory / std::format("triangle.vert{}", choice->extension);
    const auto fragment_path = directory / std::format("triangle.frag{}", choice->extension);

    auto vertex_code = read_file(vertex_path);
    if (!vertex_code) {
        return std::unexpected(
            std::move(vertex_code)
                .error()
                .context(
                    "loading the triangle vertex shader; run tools/cook_shaders.py if the cooked "
                    "outputs are missing"));
    }
    auto fragment_code = read_file(fragment_path);
    if (!fragment_code) {
        return std::unexpected(
            std::move(fragment_code).error().context("loading the triangle fragment shader"));
    }

    TriangleRenderer renderer;
    renderer.m_device = &device;

    auto vertex = device.create_shader({
        .stage = rhi::ShaderStage::Vertex,
        .format = choice->format,
        .code = *vertex_code,
        .entry_point = choice->entry_point,
        .debug_name = "triangle vertex",
    });
    if (!vertex) {
        return std::unexpected(std::move(vertex).error());
    }
    renderer.m_vertex = *vertex;

    auto fragment = device.create_shader({
        .stage = rhi::ShaderStage::Fragment,
        .format = choice->format,
        .code = *fragment_code,
        .entry_point = choice->entry_point,
        .debug_name = "triangle fragment",
    });
    if (!fragment) {
        return std::unexpected(std::move(fragment).error());
    }
    renderer.m_fragment = *fragment;

    auto pipeline = device.create_graphics_pipeline({
        .vertex_shader = renderer.m_vertex,
        .fragment_shader = renderer.m_fragment,
        // No vertex layout: the shader builds its positions from the vertex index.
        .vertex_layout = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .colour_format = device.swapchain_format(),
        .debug_name = "triangle",
    });
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    renderer.m_pipeline = *pipeline;

    ATLAS_LOG_INFO(kApp, "triangle renderer ready: shaders are {} from '{}'",
                   to_string(choice->format), directory.string());
    return renderer;
}

void TriangleRenderer::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    // Reverse of creation order, and each destroy is safe on a null or already-released
    // handle, so a partially constructed renderer cleans up correctly too.
    m_device->destroy_graphics_pipeline(m_pipeline);
    m_device->destroy_shader(m_fragment);
    m_device->destroy_shader(m_vertex);
    m_device = nullptr;
}

TriangleRenderer::~TriangleRenderer() {
    release();
}

TriangleRenderer::TriangleRenderer(TriangleRenderer&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_vertex(std::exchange(other.m_vertex, {})),
      m_fragment(std::exchange(other.m_fragment, {})),
      m_pipeline(std::exchange(other.m_pipeline, {})) {}

TriangleRenderer& TriangleRenderer::operator=(TriangleRenderer&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_vertex = std::exchange(other.m_vertex, {});
        m_fragment = std::exchange(other.m_fragment, {});
        m_pipeline = std::exchange(other.m_pipeline, {});
    }
    return *this;
}

void TriangleRenderer::draw(rhi::RenderPass& pass) const {
    ATLAS_ZONE_NAMED("draw triangle");
    pass.push_debug_group("triangle");
    pass.bind_pipeline(m_pipeline);
    pass.draw(3);
    pass.pop_debug_group();
}

}  // namespace atlas::sandbox

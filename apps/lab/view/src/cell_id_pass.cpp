// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/profile.hpp>
#include <atlas/lab/cell_id_pass.hpp>
#include <atlas/renderer/shader_loader.hpp>

#include <array>
#include <cstring>
#include <format>
#include <utility>

namespace atlas::lab {
namespace {

/// Mirrors cbuffer Chunk in cell_id.vert.hlsl, in the std140 layout the shader reads.
struct ChunkUniforms {
    std::array<float, 16> view_projection;  ///< Column-major, from uniform_elements().
    std::array<float, 4> origin_and_cell;   ///< x, y, cell size, unused
    std::array<std::uint32_t, 4> layout;    ///< cells per chunk side, first cell, unused x2
};

static_assert(sizeof(ChunkUniforms) == 96, "the shader declares a 96-byte constant buffer");

}  // namespace

Result<CellIdPass> CellIdPass::create(rhi::Device& device, const Config& config) {
    CellIdPass pass;
    pass.m_device = &device;
    auto shaders = renderer::load_shader_pair(device, config.shader_directory, "cell_id");
    if (!shaders) {
        return std::unexpected(std::move(shaders).error().context("loading the cell id shaders"));
    }
    pass.m_vertex = shaders->vertex;
    pass.m_fragment = shaders->fragment;

    // No vertex streams: everything comes from the instance index and the uniforms.
    auto pipeline = device.create_graphics_pipeline({
        .vertex_shader = pass.m_vertex,
        .fragment_shader = pass.m_fragment,
        .vertex_layout = {},
        .topology = rhi::PrimitiveTopology::TriangleStrip,
        .colour_format = rhi::TextureFormat::R32Uint,
        .blend = rhi::BlendMode::Replace,
        .debug_name = "cell id",
    });
    if (!pipeline) {
        return std::unexpected(
            std::move(pipeline).error().context("creating the cell id pipeline"));
    }
    pass.m_pipeline = *pipeline;
    return pass;
}

CellIdPass::~CellIdPass() {
    release();
}

CellIdPass::CellIdPass(CellIdPass&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_vertex(std::exchange(other.m_vertex, {})),
      m_fragment(std::exchange(other.m_fragment, {})),
      m_pipeline(std::exchange(other.m_pipeline, {})), m_target(std::exchange(other.m_target, {})),
      m_extent(std::exchange(other.m_extent, {})) {}

CellIdPass& CellIdPass::operator=(CellIdPass&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_vertex = std::exchange(other.m_vertex, {});
        m_fragment = std::exchange(other.m_fragment, {});
        m_pipeline = std::exchange(other.m_pipeline, {});
        m_target = std::exchange(other.m_target, {});
        m_extent = std::exchange(other.m_extent, {});
    }
    return *this;
}

void CellIdPass::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    if (m_target.valid()) {
        m_device->destroy_texture(m_target);
    }
    m_device->destroy_graphics_pipeline(m_pipeline);
    m_device->destroy_shader(m_fragment);
    m_device->destroy_shader(m_vertex);
    m_device = nullptr;
}

Status CellIdPass::resize(std::uint32_t pixel_width, std::uint32_t pixel_height) {
    if (pixel_width == 0 || pixel_height == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "the id target needs a non-zero size"));
    }
    if (m_target.valid() && m_extent.width == pixel_width && m_extent.height == pixel_height) {
        return ok();
    }
    auto target = m_device->create_texture({
        .width = pixel_width,
        .height = pixel_height,
        .format = rhi::TextureFormat::R32Uint,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "cell id target",
    });
    if (!target) {
        return std::unexpected(std::move(target).error().context("creating the cell id target"));
    }
    if (m_target.valid()) {
        m_device->destroy_texture(m_target);
    }
    m_target = *target;
    m_extent = {.width = pixel_width, .height = pixel_height};
    return ok();
}

Status CellIdPass::render(rhi::Frame& frame, const CellField& field,
                          std::span<const std::uint32_t> chunks) {
    ATLAS_ZONE_NAMED("cell id pass");
    if (!m_target.valid()) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "the id pass has no target; call resize first"));
    }
    auto pass = frame.begin_render_pass({
        .colour = {.texture = m_target, .load = rhi::LoadOp::Clear},
        .debug_name = "cell id pass",
    });
    if (!pass) {
        return std::unexpected(std::move(pass).error().context("beginning the cell id pass"));
    }
    pass->set_viewport(0.0F, 0.0F, static_cast<float>(m_extent.width),
                       static_cast<float>(m_extent.height));
    pass->bind_pipeline(m_pipeline);

    const GridLayout& layout = field.layout();
    ChunkUniforms uniforms{};
    uniforms.view_projection = field.camera().view_projection().uniform_elements();
    uniforms.layout[0] = layout.chunk_size();
    for (const std::uint32_t chunk : chunks) {
        const CellCoords origin = layout.chunk_origin(chunk);
        uniforms.origin_and_cell = {static_cast<float>(origin.x) * field.cell_size(),
                                    static_cast<float>(origin.y) * field.cell_size(),
                                    field.cell_size(), 0.0F};
        uniforms.layout[1] = layout.chunk_first_cell(chunk);
        pass->set_vertex_uniforms(0, std::as_bytes(std::span{&uniforms, 1}));
        pass->draw(4, layout.cells_per_chunk());
    }
    pass->end();
    return ok();
}

Result<rhi::ReadbackHandle> CellIdPass::request_pixel(std::uint32_t x, std::uint32_t y) {
    if (!m_target.valid() || x >= m_extent.width || y >= m_extent.height) {
        return std::unexpected(Error(ErrorCode::OutOfRange,
                                     std::format("pixel ({}, {}) is outside the {}x{} id target", x,
                                                 y, m_extent.width, m_extent.height)));
    }
    return m_device->request_readback(m_target,
                                      {.x = x, .y = y, .extent = {.width = 1, .height = 1}});
}

std::optional<std::uint32_t> CellIdPass::decode(const rhi::Device::Readback& pixel) {
    if (pixel.pixels.size() < 4) {
        return std::nullopt;
    }
    std::uint32_t id = 0;
    std::memcpy(&id, pixel.pixels.data(), sizeof(id));
    if (id == 0) {
        return std::nullopt;
    }
    return id - 1;
}

}  // namespace atlas::lab

// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/lab/cell_renderer.hpp>
#include <atlas/renderer/shader_loader.hpp>

#include <array>
#include <format>
#include <utility>

namespace atlas::lab {
namespace {

constexpr log::Category kLab{"lab"};

/// Mirrors cbuffer Cells in cell.vert.hlsl.
struct CellUniforms {
    std::array<float, 16> view_projection;  ///< Column-major, from uniform_elements().
    std::array<float, 4> cell_size;         ///< World units per cell; the rest unused.
    std::array<std::uint32_t, 4> layout;    ///< cells per chunk side, chunks per row, first chunk
};

static_assert(sizeof(CellUniforms) == 96, "the shader declares a 96-byte constant buffer");

}  // namespace

Result<CellRenderer> CellRenderer::create(rhi::Device& device, const Config& config) {
    CellRenderer renderer;
    renderer.m_device = &device;

    auto shaders = renderer::load_shader_pair(device, config.shader_directory, "cell");
    if (!shaders) {
        return std::unexpected(std::move(shaders).error().context("loading the cell shaders"));
    }
    renderer.m_vertex = shaders->vertex;
    renderer.m_fragment = shaders->fragment;

    // One stream, advancing per instance, four bytes wide: the colour and nothing else.
    constexpr std::array<rhi::VertexAttribute, 1> kAttributes{{
        {.location = 0, .offset = 0, .format = rhi::VertexFormat::UByte4Norm},
    }};
    const std::array<rhi::VertexStream, 1> streams{{
        {.stride = kBytesPerCell, .per_instance = true, .attributes = kAttributes},
    }};

    auto pipeline = device.create_graphics_pipeline({
        .vertex_shader = renderer.m_vertex,
        .fragment_shader = renderer.m_fragment,
        .vertex_layout = {.streams = streams},
        .topology = rhi::PrimitiveTopology::TriangleStrip,
        .colour_format = device.swapchain_format(),
        .debug_name = "cell field",
    });
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error().context("creating the cell pipeline"));
    }
    renderer.m_pipeline = *pipeline;
    renderer.m_target_format = device.swapchain_format();
    return renderer;
}

CellRenderer::~CellRenderer() {
    release();
}

CellRenderer::CellRenderer(CellRenderer&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_vertex(std::exchange(other.m_vertex, {})),
      m_fragment(std::exchange(other.m_fragment, {})),
      m_pipeline(std::exchange(other.m_pipeline, {})),
      m_instances(std::exchange(other.m_instances, {})),
      m_capacity_cells(std::exchange(other.m_capacity_cells, 0)),
      m_target_format(std::exchange(other.m_target_format, rhi::TextureFormat::Unknown)),
      m_stats(other.m_stats) {}

CellRenderer& CellRenderer::operator=(CellRenderer&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_vertex = std::exchange(other.m_vertex, {});
        m_fragment = std::exchange(other.m_fragment, {});
        m_pipeline = std::exchange(other.m_pipeline, {});
        m_instances = std::exchange(other.m_instances, {});
        m_capacity_cells = std::exchange(other.m_capacity_cells, 0);
        m_target_format = std::exchange(other.m_target_format, rhi::TextureFormat::Unknown);
        m_stats = other.m_stats;
    }
    return *this;
}

void CellRenderer::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    if (m_instances.valid()) {
        m_device->destroy_buffer(m_instances);
    }
    m_device->destroy_graphics_pipeline(m_pipeline);
    m_device->destroy_shader(m_fragment);
    m_device->destroy_shader(m_vertex);
    m_device = nullptr;
}

Status CellRenderer::set_layout(const GridLayout& layout) {
    if (layout.cell_count() == m_capacity_cells && m_instances.valid()) {
        return ok();
    }
    auto buffer = m_device->create_buffer({
        .size = static_cast<std::uint64_t>(layout.cell_count()) * kBytesPerCell,
        .usage = rhi::BufferUsage::Vertex,
        .debug_name = "cell colours",
    });
    if (!buffer) {
        return std::unexpected(std::move(buffer).error().context("sizing the cell buffer"));
    }
    if (m_instances.valid()) {
        m_device->destroy_buffer(m_instances);
    }
    m_instances = *buffer;
    m_capacity_cells = layout.cell_count();
    ATLAS_LOG_INFO(kLab, "cell renderer: {} cells, {} KiB of instance data ({} bytes each)",
                   m_capacity_cells, (m_capacity_cells * kBytesPerCell) / 1024, kBytesPerCell);
    return ok();
}

Status CellRenderer::draw(rhi::RenderPass& pass, const math::Mat4& view_projection,
                          const GridLayout& layout, float cell_size,
                          std::span<const std::uint32_t> colours, std::span<const ChunkRun> runs) {
    ATLAS_ZONE_NAMED("cell renderer draw");
    m_stats = {};
    if (runs.empty() || !m_instances.valid()) {
        return ok();
    }
    std::uint32_t total = 0;
    for (const ChunkRun& run : runs) {
        total += run.chunk_count * layout.cells_per_chunk();
    }
    if (colours.size() != total) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("{} colours for {} cells across {} run(s)",
                                                 colours.size(), total, runs.size())));
    }
    if (total > m_capacity_cells) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("{} visible cells exceeds the {} the buffer was sized for", total,
                              m_capacity_cells)));
    }
    if (pass.target_format() != m_target_format) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  "the cell pipeline was built for a different target format than this pass"));
    }

    // One upload per frame, of the visible cells only. It has to be one: stream_buffer cycles
    // the buffer, so a second upload would leave the first draw reading undefined memory. The
    // caller packs the runs in order for the same reason, so a run's colours are where this
    // expects them without a second copy.
    const auto bytes = std::as_bytes(colours);
    if (auto status = m_device->stream_buffer(m_instances, bytes); !status) {
        return std::unexpected(std::move(status).error().context("uploading cell colours"));
    }
    m_stats.bytes_uploaded = bytes.size();

    pass.bind_pipeline(m_pipeline);
    const std::uint32_t per_chunk = layout.cells_per_chunk();
    CellUniforms uniforms{};
    uniforms.view_projection = view_projection.uniform_elements();
    uniforms.cell_size = {cell_size, 0.0F, 0.0F, 0.0F};
    uniforms.layout[0] = layout.chunk_size();
    uniforms.layout[1] = layout.chunks_x();

    std::uint64_t offset = 0;
    for (const ChunkRun& run : runs) {
        const std::uint32_t cells = run.chunk_count * per_chunk;
        uniforms.layout[2] = run.first_chunk;
        pass.set_vertex_uniforms(0, std::as_bytes(std::span{&uniforms, 1}));
        // The instance index restarts at zero for each draw, which is what the shader's
        // arithmetic assumes; the offset is where this run's colours begin.
        pass.bind_vertex_buffer(0, m_instances, offset);
        pass.draw(4, cells);
        offset += static_cast<std::uint64_t>(cells) * kBytesPerCell;
        ++m_stats.draw_calls;
        m_stats.cells += cells;
    }
    return ok();
}

}  // namespace atlas::lab

// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/renderer/shader_loader.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <utility>

namespace atlas::renderer {
namespace {

constexpr log::Category kRenderer{"renderer"};

/// The four corners of a unit quad, shared by every instance.
///
/// A triangle strip, so four vertices draw two triangles with no index buffer. The order is
/// top-left, bottom-left, top-right, bottom-right, which is what a strip needs.
struct Corner {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
};

constexpr std::array<Corner, 4> kCorners{{
    {.x = 0.0F, .y = 0.0F, .u = 0.0F, .v = 0.0F},
    {.x = 0.0F, .y = 1.0F, .u = 0.0F, .v = 1.0F},
    {.x = 1.0F, .y = 0.0F, .u = 1.0F, .v = 0.0F},
    {.x = 1.0F, .y = 1.0F, .u = 1.0F, .v = 1.0F},
}};

[[nodiscard]] std::span<const std::byte> as_bytes(const void* data, std::size_t size) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a byte view of an object.
    return {reinterpret_cast<const std::byte*>(data), size};
}

}  // namespace

Result<QuadBatch> QuadBatch::create(rhi::Device& device, const Config& config) {
    if (config.capacity == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "a quad batch needs a capacity of at least one"));
    }

    QuadBatch batch;
    batch.m_device = &device;
    batch.m_capacity = config.capacity;

    auto shaders = load_shader_pair(device, config.shader_directory, "sprite");
    if (!shaders) {
        return std::unexpected(std::move(shaders).error().context("loading the sprite shaders"));
    }
    batch.m_vertex_shader = shaders->vertex;
    batch.m_fragment_shader = shaders->fragment;

    // Slot zero advances per vertex and holds the shared corners; slot one advances per
    // instance and holds what makes each quad different. That split is the whole reason one
    // draw can cover thousands of quads.
    //
    // The offsets come from `offsetof` rather than from literals. They were literals until
    // M13, and nothing cross-checked them against the struct they describe: a field inserted
    // above another moved it, the numbers stayed, and the mismatch reaches the driver rather
    // than the compiler. The shader's own declared inputs are now compared against this list
    // by tools/cook_shaders.py, which closes the other half of the same gap.
    constexpr std::array<rhi::VertexAttribute, 2> kCornerAttributes{{
        {.location = 0, .offset = offsetof(Corner, x), .format = rhi::VertexFormat::Float2},
        {.location = 1, .offset = offsetof(Corner, u), .format = rhi::VertexFormat::Float2},
    }};
    constexpr std::array<rhi::VertexAttribute, 4> kInstanceAttributes{{
        {.location = 2,
         .offset = offsetof(Instance, position_size),
         .format = rhi::VertexFormat::Float4},
        {.location = 3, .offset = offsetof(Instance, uv_rect), .format = rhi::VertexFormat::Float4},
        {.location = 4, .offset = offsetof(Instance, colour), .format = rhi::VertexFormat::Float4},
        {.location = 5,
         .offset = offsetof(Instance, rotation_pivot),
         .format = rhi::VertexFormat::Float4},
    }};
    const std::array<rhi::VertexStream, 2> streams{{
        {.stride = sizeof(Corner), .per_instance = false, .attributes = kCornerAttributes},
        {.stride = sizeof(Instance), .per_instance = true, .attributes = kInstanceAttributes},
    }};

    auto pipeline = device.create_graphics_pipeline({
        .vertex_shader = batch.m_vertex_shader,
        .fragment_shader = batch.m_fragment_shader,
        .vertex_layout = {.streams = streams},
        .topology = rhi::PrimitiveTopology::TriangleStrip,
        .colour_format = device.swapchain_format(),
        .debug_name = "quad batch",
    });
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    batch.m_pipeline = *pipeline;
    batch.m_target_format = device.swapchain_format();

    auto corner_buffer = device.create_buffer({
        .size = sizeof(kCorners),
        .usage = rhi::BufferUsage::Vertex,
        .debug_name = "quad corners",
    });
    if (!corner_buffer) {
        return std::unexpected(std::move(corner_buffer).error());
    }
    batch.m_corner_buffer = *corner_buffer;

    // Not const: context() adds to the error in place, so it needs a modifiable one.
    if (auto status = device.upload_buffer(batch.m_corner_buffer,
                                           as_bytes(kCorners.data(), sizeof(kCorners)));
        !status) {
        return std::unexpected(
            std::move(status).error().context("uploading the shared quad corners"));
    }

    auto instance_buffer = device.create_buffer({
        .size = static_cast<std::uint64_t>(config.capacity) * sizeof(Instance),
        .usage = rhi::BufferUsage::Vertex,
        .debug_name = "quad instances",
    });
    if (!instance_buffer) {
        return std::unexpected(std::move(instance_buffer).error());
    }
    batch.m_instance_buffer = *instance_buffer;

    // Reserved once, reused every frame. This is what keeps a steady-state frame free of
    // allocation; the test for that is in the benchmark harness.
    batch.m_instances.reserve(config.capacity);

    ATLAS_LOG_INFO(kRenderer, "quad batch ready: capacity {} quads ({} KiB of instance data)",
                   config.capacity,
                   (static_cast<std::uint64_t>(config.capacity) * sizeof(Instance)) / 1024);
    return batch;
}

void QuadBatch::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    m_device->destroy_buffer(m_instance_buffer);
    m_device->destroy_buffer(m_corner_buffer);
    m_device->destroy_graphics_pipeline(m_pipeline);
    m_device->destroy_shader(m_fragment_shader);
    m_device->destroy_shader(m_vertex_shader);
    m_device = nullptr;
}

QuadBatch::~QuadBatch() {
    release();
}

QuadBatch::QuadBatch(QuadBatch&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_pass(std::exchange(other.m_pass, nullptr)),
      m_vertex_shader(std::exchange(other.m_vertex_shader, {})),
      m_fragment_shader(std::exchange(other.m_fragment_shader, {})),
      m_pipeline(std::exchange(other.m_pipeline, {})),
      m_target_format(std::exchange(other.m_target_format, rhi::TextureFormat::Unknown)),
      m_corner_buffer(std::exchange(other.m_corner_buffer, {})),
      m_instance_buffer(std::exchange(other.m_instance_buffer, {})),
      m_texture(std::exchange(other.m_texture, {})), m_sampler(std::exchange(other.m_sampler, {})),
      m_instances(std::move(other.m_instances)), m_view_projection(other.m_view_projection),
      m_stats(other.m_stats), m_capacity(other.m_capacity),
      m_active(std::exchange(other.m_active, false)) {}

QuadBatch& QuadBatch::operator=(QuadBatch&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_pass = std::exchange(other.m_pass, nullptr);
        m_vertex_shader = std::exchange(other.m_vertex_shader, {});
        m_fragment_shader = std::exchange(other.m_fragment_shader, {});
        m_pipeline = std::exchange(other.m_pipeline, {});
        m_target_format = std::exchange(other.m_target_format, rhi::TextureFormat::Unknown);
        m_corner_buffer = std::exchange(other.m_corner_buffer, {});
        m_instance_buffer = std::exchange(other.m_instance_buffer, {});
        m_texture = std::exchange(other.m_texture, {});
        m_sampler = std::exchange(other.m_sampler, {});
        m_instances = std::move(other.m_instances);
        m_view_projection = other.m_view_projection;
        m_stats = other.m_stats;
        m_capacity = other.m_capacity;
        m_active = std::exchange(other.m_active, false);
    }
    return *this;
}

void QuadBatch::begin(rhi::RenderPass& pass, const math::Mat4& view_projection) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT_MSG(!m_active, "begin called on a batch that is already active");

    // A pipeline is built for one colour format. Before offscreen targets existed every
    // pass drew into the swapchain and this could not be wrong; now a caller can hand over
    // a pass aimed at a texture of a different format. Metal aborts the process on that, so
    // it is caught here with a message naming both formats.
    if (m_pipeline.valid() && pass.target_format() != m_target_format) {
        ATLAS_LOG_ERROR(kRenderer,
                        "this batch draws into {} and the pass targets {}; nothing will be "
                        "drawn. Create a batch for the pass's format.",
                        rhi::to_string(m_target_format), rhi::to_string(pass.target_format()));
        m_pass = nullptr;
        m_stats = BatchStats{};
        m_instances.clear();
        m_active = true;
        return;
    }

    m_pass = &pass;
    m_view_projection = view_projection;
    m_stats = BatchStats{};
    m_instances.clear();
    m_texture = {};
    m_sampler = {};
    m_active = true;
}

void QuadBatch::set_texture(rhi::TextureHandle texture, rhi::SamplerHandle sampler) {
    ATLAS_ASSERT_MSG(m_active, "set_texture called outside begin and end");

    if (texture == m_texture && sampler == m_sampler) {
        return;
    }

    // One draw call reads one texture, so changing it means drawing what is pending first.
    if (!m_instances.empty()) {
        flush();
        ++m_stats.texture_switches;
    }

    m_texture = texture;
    m_sampler = sampler;
}

void QuadBatch::add(const Quad& quad) {
    ATLAS_ASSERT_MSG(m_active, "add called outside begin and end");

    if (m_instances.size() >= m_capacity) {
        flush();
        ++m_stats.capacity_flushes;
    }

    m_instances.push_back(Instance{
        .position_size = {quad.bounds.position.x, quad.bounds.position.y, quad.bounds.size.x,
                          quad.bounds.size.y},
        .uv_rect = {quad.uv.position.x, quad.uv.position.y, quad.uv.size.x, quad.uv.size.y},
        .colour = {quad.colour.r, quad.colour.g, quad.colour.b, quad.colour.a},
        // The angle goes across as an angle. Computing its sine and cosine here would be a
        // pair of trigonometric calls per quad on this thread; the graphics device does them
        // per vertex, four at a time, on hardware built for it.
        .rotation_pivot = {quad.rotation, quad.pivot.x, quad.pivot.y, 0.0F},
    });
    ++m_stats.quads;
}

void QuadBatch::add(std::span<const Quad> quads) {
    ATLAS_ASSERT_MSG(m_active, "add called outside begin and end");

    for (const auto& quad : quads) {
        add(quad);
    }
}

void QuadBatch::flush() {
    if (m_instances.empty() || m_pass == nullptr || m_device == nullptr) {
        return;
    }
    ATLAS_ZONE_NAMED("QuadBatch::flush");
    ATLAS_ASSERT_MAIN_THREAD();

    const std::size_t bytes = m_instances.size() * sizeof(Instance);

    if (const auto status =
            m_device->stream_buffer(m_instance_buffer, as_bytes(m_instances.data(), bytes));
        !status) {
        ATLAS_LOG_ERROR(kRenderer, "uploading {} quads failed: {}", m_instances.size(),
                        status.error());
        m_instances.clear();
        return;
    }

    m_pass->bind_pipeline(m_pipeline);
    // uniform_elements, not elements: a shader reads a uniform matrix column-major, and
    // uploading the row-major storage would drop the translation and leave a varying w,
    // which warps the whole field into a wedge.
    const auto uniform = m_view_projection.uniform_elements();
    m_pass->set_vertex_uniforms(0, as_bytes(uniform.data(), sizeof(float) * 16));
    m_pass->bind_vertex_buffer(0, m_corner_buffer);
    m_pass->bind_vertex_buffer(1, m_instance_buffer);

    const std::array<rhi::TextureSamplerBinding, 1> bindings{
        {{.texture = m_texture, .sampler = m_sampler}}};
    m_pass->bind_fragment_samplers(0, bindings);

    // Four vertices of a triangle strip, once per instance.
    m_pass->draw(4, static_cast<std::uint32_t>(m_instances.size()));

    ++m_stats.draw_calls;
    m_stats.bytes_uploaded += bytes;

    // clear() keeps the capacity, which is what makes the next frame allocation-free.
    m_instances.clear();
}

BatchStats QuadBatch::end() {
    ATLAS_ASSERT_MSG(m_active, "end called on a batch that was not active");

    flush();
    m_active = false;
    m_pass = nullptr;
    return m_stats;
}

}  // namespace atlas::renderer

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Draws many textured rectangles in as few calls as possible.
///
/// Each quad is one instance: four shared corners from a vertex buffer, plus the position,
/// size, texture rectangle and colour that make this quad different. Ten thousand quads are
/// one buffer upload and one draw, not ten thousand of anything.
///
/// Batches break when the texture changes, because a draw call can only read one. Callers
/// that care about the number of draws should submit in texture order; the batch reports
/// what it did so that the cost is visible rather than guessed at.
///
/// Thread affinity: main thread only, like everything in the renderer.

#include <atlas/core/result.hpp>
#include <atlas/math/matrix.hpp>
#include <atlas/math/vector.hpp>
#include <atlas/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace atlas::renderer {

/// One rectangle to draw.
struct Quad {
    /// Position of the top-left corner, and size, in world units.
    math::Rect bounds;

    /// Region of the texture to show, in normalised coordinates. The default is the whole
    /// texture.
    math::Rect uv{.position = {0.0F, 0.0F}, .size = {1.0F, 1.0F}};

    /// Multiplied with the sampled texel. Opaque white leaves the texture unchanged.
    rhi::Colour colour{.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F};
};

/// What one frame of batching cost.
struct BatchStats {
    std::uint32_t quads = 0;
    std::uint32_t draw_calls = 0;
    /// Times the batch was flushed because the texture changed.
    std::uint32_t texture_switches = 0;
    /// Times the batch was flushed because it filled up.
    std::uint32_t capacity_flushes = 0;
    std::uint64_t bytes_uploaded = 0;
};

class QuadBatch {
  public:
    /// Quads per draw call.
    ///
    /// Sized so that the instance buffer is a few hundred kilobytes: large enough that a
    /// realistic scene is one or two draws, small enough to stay reasonable to upload.
    static constexpr std::uint32_t kDefaultCapacity = 16384;

    struct Config {
        std::uint32_t capacity = kDefaultCapacity;
        /// Where the cooked shaders live.
        std::string_view shader_directory = "assets/cooked/shaders";
    };

    [[nodiscard]] static Result<QuadBatch> create(rhi::Device& device, const Config& config);

    /// An empty batch that owns nothing.
    ///
    /// The state a moved-from batch is left in, and what a member of a type that is built in
    /// two stages holds before its batch is created. Every operation on one is a no-op
    /// rather than undefined.
    QuadBatch() = default;

    ~QuadBatch();

    QuadBatch(const QuadBatch&) = delete;
    QuadBatch& operator=(const QuadBatch&) = delete;
    QuadBatch(QuadBatch&& other) noexcept;
    QuadBatch& operator=(QuadBatch&& other) noexcept;

    /// Start a frame of batching. Clears the statistics.
    void begin(rhi::RenderPass& pass, const math::Mat4& view_projection);

    /// Choose the texture subsequent quads are drawn with.
    ///
    /// Changing it flushes whatever is pending, because one draw reads one texture.
    void set_texture(rhi::TextureHandle texture, rhi::SamplerHandle sampler);

    /// Queue one quad. Flushes automatically when the batch is full.
    void add(const Quad& quad);

    /// Queue several quads at once, which avoids a per-quad call for bulk submission.
    void add(std::span<const Quad> quads);

    /// Draw whatever is pending. Called by end(), and by set_texture when the texture
    /// changes; a caller rarely needs it directly.
    void flush();

    /// Finish the frame and return what it cost.
    [[nodiscard]] BatchStats end();

    [[nodiscard]] const BatchStats& stats() const noexcept { return m_stats; }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return m_capacity; }

    /// Quads queued but not yet drawn.
    [[nodiscard]] std::uint32_t pending() const noexcept {
        return static_cast<std::uint32_t>(m_instances.size());
    }

  private:
    void release() noexcept;

    /// What the shader reads per instance. The layout is declared to the pipeline, so the
    /// field order here is part of that contract.
    struct Instance {
        std::array<float, 4> position_size;  ///< x, y, width, height
        std::array<float, 4> uv_rect;        ///< u, v, width, height
        std::array<float, 4> colour;         ///< red, green, blue, alpha
    };

    static_assert(sizeof(Instance) == 48, "the vertex layout declares a 48-byte instance");

    rhi::Device* m_device = nullptr;
    rhi::RenderPass* m_pass = nullptr;

    rhi::ShaderHandle m_vertex_shader;
    rhi::ShaderHandle m_fragment_shader;
    rhi::GraphicsPipelineHandle m_pipeline;
    rhi::BufferHandle m_corner_buffer;
    rhi::BufferHandle m_instance_buffer;

    rhi::TextureHandle m_texture;
    rhi::SamplerHandle m_sampler;

    /// Reused between frames, so a steady state performs no allocation.
    std::vector<Instance> m_instances;

    math::Mat4 m_view_projection;
    BatchStats m_stats;
    std::uint32_t m_capacity = kDefaultCapacity;
    bool m_active = false;
};

}  // namespace atlas::renderer

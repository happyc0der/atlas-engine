// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Drawing grid cells with four bytes each.
///
/// The quad batcher takes forty-eight bytes per instance and is right to: a sprite can be
/// anywhere, any size, showing any part of any texture. A grid cell can be none of those
/// things. Its rectangle follows from its index, because cells are stored chunk-major and a
/// run of consecutive chunks is a contiguous range of them, so the only thing that has to
/// reach the graphics processor is the colour.
///
/// That is what this does, and it is the identifier pass's trick applied to the picture rather
/// than to picking — the second call site for deriving geometry from an instance index, which
/// is what justified generalising it from one shader to two rather than three.
///
/// Thread affinity: main thread; it owns device resources.

#include <atlas/core/result.hpp>
#include <atlas/lab/grid_layout.hpp>
#include <atlas/math/matrix.hpp>
#include <atlas/rhi/device.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace atlas::lab {

/// A run of consecutive chunks, which is a contiguous range of cells and therefore one draw.
struct ChunkRun {
    std::uint32_t first_chunk = 0;
    std::uint32_t chunk_count = 0;
};

struct CellDrawStats {
    std::uint32_t cells = 0;
    std::uint32_t draw_calls = 0;
    std::uint64_t bytes_uploaded = 0;
};

class CellRenderer {
  public:
    struct Config {
        std::string_view shader_directory = "assets/cooked/shaders";
    };

    [[nodiscard]] static Result<CellRenderer> create(rhi::Device& device, const Config& config);

    CellRenderer() = default;
    ~CellRenderer();
    CellRenderer(const CellRenderer&) = delete;
    CellRenderer& operator=(const CellRenderer&) = delete;
    CellRenderer(CellRenderer&& other) noexcept;
    CellRenderer& operator=(CellRenderer&& other) noexcept;

    /// Size the instance buffer for a layout. Allocates once; drawing never does.
    ///
    /// Failure: whatever create_buffer reports; the previous buffer is kept.
    [[nodiscard]] Status set_layout(const GridLayout& layout);

    /// Colours for every cell of the grid, in cell order, as packed red-green-blue-alpha bytes.
    /// Borrowed for the call only.
    [[nodiscard]] Status draw(rhi::RenderPass& pass, const math::Mat4& view_projection,
                              const GridLayout& layout, float cell_size,
                              std::span<const std::uint32_t> colours,
                              std::span<const ChunkRun> runs);

    [[nodiscard]] const CellDrawStats& stats() const noexcept { return m_stats; }

    /// What one cell costs on the wire. Four bytes, which is the point of this class.
    static constexpr std::uint32_t kBytesPerCell = 4;

  private:
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    rhi::ShaderHandle m_vertex;
    rhi::ShaderHandle m_fragment;
    rhi::GraphicsPipelineHandle m_pipeline;
    rhi::BufferHandle m_instances;
    std::uint32_t m_capacity_cells = 0;
    rhi::TextureFormat m_target_format = rhi::TextureFormat::Unknown;
    CellDrawStats m_stats;
};

}  // namespace atlas::lab

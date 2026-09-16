// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The field of cells on screen: geometry built once from the layout, coloured per frame
/// from a snapshot band through a palette, culled per chunk.
///
/// Geometry is a pure function of the layout and is never rebuilt for a map mode. A mode is
/// a (band, palette) pair; switching one changes which span and which palette the draw loop
/// reads, and nothing is reallocated. That is tested directly, not assumed.
///
/// Culling is per chunk against the camera's visible bounds. Each visible chunk is one
/// contiguous run of cells, coloured into a preallocated scratch and handed to the batch in
/// one call. Nothing here allocates per frame once the layout is set.
///
/// Thread affinity: main thread; it owns device resources.

#include <atlas/core/result.hpp>
#include <atlas/lab/grid_layout.hpp>
#include <atlas/lab/snapshot.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/math/vector.hpp>
#include <atlas/platform/event.hpp>
#include <atlas/platform/input.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/rhi/device.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace atlas::lab {

using Palette = std::array<rhi::Colour, 256>;

/// The four palettes, one per map mode, built once. Presentation only: floats are fine here
/// because nothing authoritative reads them.
[[nodiscard]] const Palette& palette_for(MapMode mode) noexcept;

class CellField {
  public:
    struct Config {
        std::string_view shader_directory = "assets/cooked/shaders";
        /// World units per cell. The camera works in world units, so this sets how far a
        /// pan moves in cells.
        float cell_size = 8.0F;
    };

    struct DrawStats {
        renderer::BatchStats batch;
        std::uint32_t visible_chunks = 0;
        std::uint32_t visible_cells = 0;
    };

    [[nodiscard]] static Result<CellField> create(rhi::Device& device, const Config& config);

    CellField() = default;
    ~CellField();
    CellField(const CellField&) = delete;
    CellField& operator=(const CellField&) = delete;
    CellField(CellField&& other) noexcept;
    CellField& operator=(CellField&& other) noexcept;

    /// Build the geometry for a layout. O(cells), once; and again only after a load whose
    /// layout differs.
    void set_layout(const GridLayout& layout);

    [[nodiscard]] const GridLayout& layout() const noexcept { return m_layout; }

    void resize(std::uint32_t pixel_width, std::uint32_t pixel_height);
    /// Fit the whole grid in view.
    void reset_camera() noexcept;
    /// Pan and zoom from input. `display_scale` converts the pointer's logical coordinates
    /// into the pixels the camera's viewport is measured in; on a high-density display the
    /// two differ, and a zoom anchored in the wrong units drifts.
    void update(const platform::InputState& input, std::span<const platform::Event> events,
                float display_scale);

    /// Draw the visible chunks of `snapshot` in `mode`. The snapshot's layout must equal
    /// this field's; a mismatch draws nothing and is reported by the returned zero counts.
    [[nodiscard]] DrawStats draw(rhi::RenderPass& pass, const CellSnapshot& snapshot, MapMode mode);

    /// The chunks the last draw found visible.
    [[nodiscard]] std::span<const std::uint32_t> visible_chunks() const noexcept {
        return m_visible;
    }

    /// The chunks visible to the camera as it is now, into a caller-owned vector. The same
    /// test draw() applies, so the picking pass and the picture agree on what is on screen.
    void cull(std::vector<std::uint32_t>& out) const;

    [[nodiscard]] math::OrthoCamera& camera() noexcept { return m_camera; }

    [[nodiscard]] const math::OrthoCamera& camera() const noexcept { return m_camera; }

    [[nodiscard]] float cell_size() const noexcept { return m_cell_size; }

    /// The world-space rectangle of a chunk; what culling tests.
    [[nodiscard]] math::Rect chunk_bounds(std::uint32_t chunk) const noexcept;

    /// The analytic inverse of the picking pass: which cell is under a screen point, or none.
    /// The GPU pass is tested against this.
    [[nodiscard]] std::optional<std::uint32_t> cell_at_screen(math::Vec2 screen) const noexcept;

    /// The geometry's identity, for the test that a mode switch rebuilds nothing.
    [[nodiscard]] const renderer::Quad* geometry_data() const noexcept { return m_quads.data(); }

    [[nodiscard]] std::size_t geometry_size() const noexcept { return m_quads.size(); }

  private:
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    renderer::QuadBatch m_batch;
    rhi::TextureHandle m_white;
    rhi::SamplerHandle m_sampler;
    GridLayout m_layout;
    float m_cell_size = 8.0F;
    math::OrthoCamera m_camera;
    std::uint32_t m_pixel_width = 1;
    std::uint32_t m_pixel_height = 1;
    std::vector<renderer::Quad> m_quads;    ///< Geometry, in cell order; never recoloured.
    std::vector<renderer::Quad> m_scratch;  ///< One chunk's worth, coloured per draw.
    std::vector<std::uint32_t> m_visible;
    bool m_dragging = false;
};

}  // namespace atlas::lab

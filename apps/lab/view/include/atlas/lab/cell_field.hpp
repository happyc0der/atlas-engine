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
/// Culling is per chunk against the camera's visible bounds. Consecutive visible chunks are
/// merged into runs, and a run is a contiguous range of cells because of the chunk-major
/// ordering, so it is one draw. Colours are packed run by run into a buffer that is uploaded
/// once and is four bytes a cell: the rectangle follows from the instance index, so nothing
/// else has to be sent. Nothing here allocates per frame once the layout is set.
///
/// Thread affinity: main thread; it owns device resources.

#include <atlas/app/camera_controller.hpp>
#include <atlas/core/result.hpp>
#include <atlas/lab/cell_renderer.hpp>
#include <atlas/lab/grid_layout.hpp>
#include <atlas/lab/snapshot.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/math/vector.hpp>
#include <atlas/platform/event.hpp>
#include <atlas/platform/input.hpp>
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
        CellDrawStats cells;
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

    /// Adopt a layout, sizing the colour buffer for it.
    ///
    /// Failure: whatever sizing the instance buffer reports. There is no per-cell geometry to
    /// build: a cell's rectangle is derived in the shader from its index, so this is O(1) work
    /// plus one allocation, where it used to hold forty-eight bytes of geometry per cell.
    [[nodiscard]] Status set_layout(const GridLayout& layout);

    [[nodiscard]] const GridLayout& layout() const noexcept { return m_layout; }

    void resize(std::uint32_t pixel_width, std::uint32_t pixel_height);
    /// Fit the whole grid in view.
    void reset_camera() noexcept;
    /// Pan and zoom from input. `display_scale` converts the pointer's logical coordinates
    /// into the pixels the camera's viewport is measured in; on a high-density display the
    /// two differ, and a zoom anchored in the wrong units drifts.
    void update(const platform::InputState& input, std::span<const platform::Event> events,
                float display_scale, float dt_seconds, bool mouse_allowed);

    /// Draw the visible chunks of `snapshot` in `mode`. The snapshot's layout must equal
    /// this field's; a mismatch draws nothing and is reported by the returned zero counts.
    [[nodiscard]] DrawStats draw(rhi::RenderPass& pass, const CellSnapshot& snapshot, MapMode mode);

    /// The chunks the last draw found visible.
    [[nodiscard]] std::span<const std::uint32_t> visible_chunks() const noexcept {
        return m_visible;
    }

    /// The runs the last draw submitted: consecutive visible chunks merged.
    [[nodiscard]] std::span<const ChunkRun> visible_runs() const noexcept { return m_runs; }

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

    /// What one cell costs on the wire, and where its colours live. For the test that a mode
    /// switch rebuilds nothing: there is no geometry to rebuild any more, and the colour buffer
    /// neither moves nor changes size.
    [[nodiscard]] static constexpr std::uint32_t bytes_per_cell() noexcept {
        return CellRenderer::kBytesPerCell;
    }

    [[nodiscard]] const std::uint32_t* colours_data() const noexcept { return m_colours.data(); }

    [[nodiscard]] std::size_t colours_capacity() const noexcept { return m_colours.size(); }

  private:
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    CellRenderer m_renderer;
    GridLayout m_layout;
    float m_cell_size = 8.0F;
    math::OrthoCamera m_camera;
    std::uint32_t m_pixel_width = 1;
    std::uint32_t m_pixel_height = 1;
    std::vector<std::uint32_t> m_colours;  ///< Visible cells, packed run by run, four bytes each.
    std::vector<std::uint32_t> m_visible;
    std::vector<ChunkRun> m_runs;
    app::CameraControls m_camera_controls{.drag_button = platform::MouseButton::Right};
    app::CameraController m_camera_controller{m_camera_controls};
};

}  // namespace atlas::lab

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The shape of the synthetic grid, and the one ordering decision everything else rests on.
///
/// Cells are stored chunk-major: all of chunk 0's cells contiguously, then chunk 1's, and
/// row-major within a chunk. One decision that pays three times. A visible chunk is one
/// contiguous range to submit. A cell's identifier is derivable from its instance index plus
/// a per-chunk base, so the renderer's instance needs no identifier field. And parallel work
/// in M8 gets disjoint contiguous ranges for free. The cost is that the grid must be a whole
/// number of chunks, which create() refuses otherwise, naming both numbers.
///
/// Holds no math type on purpose: this header is part of the library that may link only the
/// simulation module.
///
/// Thread affinity: immutable after construction; safe to read from any thread.

#include <atlas/core/result.hpp>

#include <cstdint>

namespace atlas::lab {

struct CellCoords {
    std::uint32_t x = 0;
    std::uint32_t y = 0;

    friend constexpr bool operator==(const CellCoords&, const CellCoords&) noexcept = default;
};

class GridLayout {
  public:
    /// 2048 squared: four times the milestone's million-cell target, so the target is not the
    /// edge of what is representable. Every table's row bound derives from this.
    static constexpr std::uint32_t kMaxCells = 4'194'304;
    static constexpr std::uint32_t kMaxChunkSize = 256;

    constexpr GridLayout() = default;

    /// Failure: InvalidArgument when a dimension is zero, not a multiple of the chunk size,
    /// or the cell count exceeds kMaxCells. The message names the offending numbers.
    [[nodiscard]] static Result<GridLayout> create(std::uint32_t width, std::uint32_t height,
                                                   std::uint32_t chunk_size);

    [[nodiscard]] constexpr std::uint32_t width() const noexcept { return m_width; }

    [[nodiscard]] constexpr std::uint32_t height() const noexcept { return m_height; }

    [[nodiscard]] constexpr std::uint32_t chunk_size() const noexcept { return m_chunk_size; }

    [[nodiscard]] constexpr std::uint32_t chunks_x() const noexcept {
        return m_chunk_size == 0 ? 0 : m_width / m_chunk_size;
    }

    [[nodiscard]] constexpr std::uint32_t chunks_y() const noexcept {
        return m_chunk_size == 0 ? 0 : m_height / m_chunk_size;
    }

    [[nodiscard]] constexpr std::uint32_t chunk_count() const noexcept {
        return chunks_x() * chunks_y();
    }

    [[nodiscard]] constexpr std::uint32_t cells_per_chunk() const noexcept {
        return m_chunk_size * m_chunk_size;
    }

    [[nodiscard]] constexpr std::uint32_t cell_count() const noexcept { return m_width * m_height; }

    [[nodiscard]] constexpr bool empty() const noexcept { return cell_count() == 0; }

    [[nodiscard]] constexpr bool contains(std::uint32_t x, std::uint32_t y) const noexcept {
        return x < m_width && y < m_height;
    }

    /// The chunk-major index of the cell at (x, y). Precondition: contains(x, y).
    ///
    /// Every function below that divides tests its own divisor first. The preconditions
    /// already exclude those calls on a constructed layout, but a default-constructed layout
    /// is representable, and a divide by zero must not be.
    [[nodiscard]] constexpr std::uint32_t cell_index(std::uint32_t x,
                                                     std::uint32_t y) const noexcept {
        if (m_chunk_size == 0) {
            return 0;
        }
        const std::uint32_t chunk = ((y / m_chunk_size) * chunks_x()) + (x / m_chunk_size);
        const std::uint32_t within = ((y % m_chunk_size) * m_chunk_size) + (x % m_chunk_size);
        return (chunk * cells_per_chunk()) + within;
    }

    /// The inverse of cell_index. Precondition: index < cell_count().
    [[nodiscard]] constexpr CellCoords cell_coords(std::uint32_t index) const noexcept {
        const std::uint32_t per_chunk = cells_per_chunk();
        const std::uint32_t per_row = chunks_x();
        if (per_chunk == 0 || per_row == 0) {
            return {};
        }
        const std::uint32_t chunk = index / per_chunk;
        const std::uint32_t within = index % per_chunk;
        const std::uint32_t chunk_x = chunk % per_row;
        const std::uint32_t chunk_y = chunk / per_row;
        return CellCoords{
            .x = (chunk_x * m_chunk_size) + (within % m_chunk_size),
            .y = (chunk_y * m_chunk_size) + (within / m_chunk_size),
        };
    }

    [[nodiscard]] constexpr std::uint32_t chunk_of(std::uint32_t index) const noexcept {
        const std::uint32_t per_chunk = cells_per_chunk();
        return per_chunk == 0 ? 0 : index / per_chunk;
    }

    [[nodiscard]] constexpr std::uint32_t chunk_first_cell(std::uint32_t chunk) const noexcept {
        return chunk * cells_per_chunk();
    }

    /// The top-left cell of a chunk, in grid coordinates.
    [[nodiscard]] constexpr CellCoords chunk_origin(std::uint32_t chunk) const noexcept {
        const std::uint32_t per_row = chunks_x();
        if (per_row == 0) {
            return {};
        }
        return CellCoords{
            .x = (chunk % per_row) * m_chunk_size,
            .y = (chunk / per_row) * m_chunk_size,
        };
    }

    friend constexpr bool operator==(const GridLayout&, const GridLayout&) noexcept = default;

  private:
    constexpr GridLayout(std::uint32_t width, std::uint32_t height,
                         std::uint32_t chunk_size) noexcept
        : m_width(width), m_height(height), m_chunk_size(chunk_size) {}

    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_chunk_size = 0;
};

}  // namespace atlas::lab

// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/grid_layout.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::ErrorCode;
using atlas::lab::CellCoords;
using atlas::lab::GridLayout;

TEST_CASE("a layout must be a whole number of chunks", "[lab][layout]") {
    const auto odd = GridLayout::create(10, 8, 4);
    REQUIRE_FALSE(odd.has_value());
    CHECK(odd.error().code() == ErrorCode::InvalidArgument);
    CHECK(odd.error().to_string().contains("10x8"));

    CHECK_FALSE(GridLayout::create(0, 8, 4).has_value());
    CHECK_FALSE(GridLayout::create(8, 8, 0).has_value());
    CHECK_FALSE(GridLayout::create(8, 8, GridLayout::kMaxChunkSize + 1).has_value());
    CHECK_FALSE(GridLayout::create(4096, 4096, 64).has_value());  // 16M cells, over the limit
    CHECK(GridLayout::create(2048, 2048, 64).has_value());        // exactly the limit
}

TEST_CASE("cell_index and cell_coords are inverses over the whole grid", "[lab][layout]") {
    const auto layout = GridLayout::create(12, 8, 4).value();
    REQUIRE(layout.cell_count() == 96);
    REQUIRE(layout.chunk_count() == 6);
    std::vector<bool> seen(layout.cell_count(), false);
    for (std::uint32_t y = 0; y < layout.height(); ++y) {
        for (std::uint32_t x = 0; x < layout.width(); ++x) {
            const std::uint32_t index = layout.cell_index(x, y);
            REQUIRE(index < layout.cell_count());
            CHECK_FALSE(seen[index]);  // a bijection: no two cells share an index
            seen[index] = true;
            CHECK(layout.cell_coords(index) == CellCoords{x, y});
        }
    }
}

TEST_CASE("a chunk's cells are one contiguous range", "[lab][layout]") {
    // This is the property the renderer and the picking pass both rely on.
    const auto layout = GridLayout::create(12, 8, 4).value();
    for (std::uint32_t chunk = 0; chunk < layout.chunk_count(); ++chunk) {
        const std::uint32_t first = layout.chunk_first_cell(chunk);
        const CellCoords origin = layout.chunk_origin(chunk);
        for (std::uint32_t i = 0; i < layout.cells_per_chunk(); ++i) {
            const CellCoords at = layout.cell_coords(first + i);
            CHECK(layout.chunk_of(first + i) == chunk);
            CHECK(at.x >= origin.x);
            CHECK(at.x < origin.x + layout.chunk_size());
            CHECK(at.y >= origin.y);
            CHECK(at.y < origin.y + layout.chunk_size());
        }
    }
    // Chunk 1 sits to the right of chunk 0, not below it.
    CHECK(layout.chunk_origin(1) == CellCoords{4, 0});
    CHECK(layout.chunk_origin(3) == CellCoords{0, 4});
}

TEST_CASE("a default layout is empty and harmless", "[lab][layout]") {
    constexpr GridLayout empty;
    STATIC_REQUIRE(empty.empty());
    STATIC_REQUIRE(empty.chunk_count() == 0);
    STATIC_REQUIRE(empty.cells_per_chunk() == 0);
}

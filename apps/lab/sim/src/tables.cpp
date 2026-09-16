// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/lab/tables.hpp>

#include <format>
#include <memory>
#include <utility>

namespace atlas::lab {
namespace {

template <typename T>
[[nodiscard]] std::span<const std::byte> column_bytes(const std::vector<T>& column) noexcept {
    return std::as_bytes(std::span{column});
}

template <typename T> [[nodiscard]] T& typed_table(sim::World& world, sim::TableId id) noexcept {
    auto* table = dynamic_cast<T*>(world.table(id));
    ATLAS_ASSERT_MSG(table != nullptr, "table id does not name a lab table of the expected type");
    return *table;
}

template <typename T>
[[nodiscard]] const T& typed_table(const sim::World& world, sim::TableId id) noexcept {
    const auto* table = dynamic_cast<const T*>(world.table(id));
    ATLAS_ASSERT_MSG(table != nullptr, "table id does not name a lab table of the expected type");
    return *table;
}

template <typename T>
[[nodiscard]] Status read_column(sim::SaveReader& reader, std::size_t rows, std::vector<T>& out,
                                 Result<T> (sim::SaveReader::*read)()) {
    std::vector<T> loaded;
    loaded.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        auto value = (reader.*read)();
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        loaded.push_back(*value);
    }
    out = std::move(loaded);
    return ok();
}

}  // namespace

// ------------------------------------------------------------------------------ GridTable

void GridTable::hash_into(Hasher& hasher) const {
    hasher.add(width).add(height).add(chunk_size).add(seed);
}

void GridTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u32(width);
    writer.write_u32(height);
    writer.write_u32(chunk_size);
    writer.write_u64(seed);
}

Status GridTable::read_from(sim::SaveReader& reader) {
    auto w = reader.read_u32();
    auto h = reader.read_u32();
    auto c = reader.read_u32();
    auto s = reader.read_u64();
    if (!w || !h || !c || !s) {
        return std::unexpected(Error(ErrorCode::MalformedData, "the grid row is truncated"));
    }
    // Refused here, so a save describing an impossible grid never reaches the other tables.
    if (auto layout = GridLayout::create(*w, *h, *c); !layout) {
        return std::unexpected(std::move(layout).error().context("the saved grid row"));
    }
    width = *w;
    height = *h;
    chunk_size = *c;
    seed = *s;
    return ok();
}

void GridTable::clear() {
    width = 0;
    height = 0;
    chunk_size = 0;
    seed = 0;
}

Result<GridLayout> GridTable::layout() const {
    return GridLayout::create(width, height, chunk_size);
}

// ------------------------------------------------------------------------------ CellTable

void CellTable::resize(std::size_t rows) {
    region_value.assign(rows, 0);
    owner_index.assign(rows, 0);
    color_index.assign(rows, 0);
}

void CellTable::hash_into(Hasher& hasher) const {
    hasher.add(static_cast<std::uint64_t>(region_value.size()));
    hasher.add(column_bytes(region_value));
    hasher.add(column_bytes(owner_index));
    hasher.add(column_bytes(color_index));
}

void CellTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u64(static_cast<std::uint64_t>(region_value.size()));
    for (const auto v : region_value) {
        writer.write_u32(v);
    }
    for (const auto v : owner_index) {
        writer.write_u16(v);
    }
    for (const auto v : color_index) {
        writer.write_u8(v);
    }
}

Status CellTable::read_from(sim::SaveReader& reader) {
    auto rows = reader.read_count(kMaxRows, 7);
    if (!rows) {
        return std::unexpected(std::move(rows).error().context("cells"));
    }
    std::vector<std::uint32_t> region;
    std::vector<std::uint16_t> owner;
    std::vector<std::uint8_t> color;
    if (auto s = read_column(reader, *rows, region, &sim::SaveReader::read_u32); !s) {
        return std::unexpected(std::move(s).error().context("cells.region_value"));
    }
    if (auto s = read_column(reader, *rows, owner, &sim::SaveReader::read_u16); !s) {
        return std::unexpected(std::move(s).error().context("cells.owner_index"));
    }
    if (auto s = read_column(reader, *rows, color, &sim::SaveReader::read_u8); !s) {
        return std::unexpected(std::move(s).error().context("cells.color_index"));
    }
    for (std::size_t i = 0; i < *rows; ++i) {
        if (owner[i] >= kOwnerCount || color[i] >= kColorCount) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("cell {} has owner {} and colour {}; the limits are {} and {}", i,
                                  owner[i], color[i], kOwnerCount, kColorCount)));
        }
    }
    region_value = std::move(region);
    owner_index = std::move(owner);
    color_index = std::move(color);
    return ok();
}

void CellTable::clear() {
    region_value.clear();
    owner_index.clear();
    color_index.clear();
}

// ------------------------------------------------------------------------ PopulationTable

void PopulationTable::resize(std::size_t rows) {
    population_value.assign(rows, 0);
}

void PopulationTable::hash_into(Hasher& hasher) const {
    hasher.add(static_cast<std::uint64_t>(population_value.size()));
    hasher.add(column_bytes(population_value));
}

void PopulationTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u64(static_cast<std::uint64_t>(population_value.size()));
    for (const auto v : population_value) {
        writer.write_u32(v);
    }
}

Status PopulationTable::read_from(sim::SaveReader& reader) {
    auto rows = reader.read_count(kMaxRows, 4);
    if (!rows) {
        return std::unexpected(std::move(rows).error().context("population"));
    }
    std::vector<std::uint32_t> loaded;
    if (auto s = read_column(reader, *rows, loaded, &sim::SaveReader::read_u32); !s) {
        return std::unexpected(std::move(s).error().context("population.population_value"));
    }
    for (std::size_t i = 0; i < *rows; ++i) {
        if (loaded[i] > kPopulationCap) {
            return std::unexpected(Error(ErrorCode::MalformedData,
                                         std::format("population {} is {}, over the cap of {}", i,
                                                     loaded[i], kPopulationCap)));
        }
    }
    population_value = std::move(loaded);
    return ok();
}

void PopulationTable::clear() {
    population_value.clear();
}

// ----------------------------------------------------------------------------- ChunkTable

void ChunkTable::resize(std::size_t rows) {
    owner_index.assign(rows, 0);
}

void ChunkTable::hash_into(Hasher& hasher) const {
    hasher.add(static_cast<std::uint64_t>(owner_index.size()));
    hasher.add(column_bytes(owner_index));
}

void ChunkTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u64(static_cast<std::uint64_t>(owner_index.size()));
    for (const auto v : owner_index) {
        writer.write_u16(v);
    }
}

Status ChunkTable::read_from(sim::SaveReader& reader) {
    auto rows = reader.read_count(kMaxRows, 2);
    if (!rows) {
        return std::unexpected(std::move(rows).error().context("chunks"));
    }
    std::vector<std::uint16_t> loaded;
    if (auto s = read_column(reader, *rows, loaded, &sim::SaveReader::read_u16); !s) {
        return std::unexpected(std::move(s).error().context("chunks.owner_index"));
    }
    for (std::size_t i = 0; i < *rows; ++i) {
        if (loaded[i] >= kOwnerCount) {
            return std::unexpected(Error(
                ErrorCode::MalformedData,
                std::format("chunk {} has owner {}; the limit is {}", i, loaded[i], kOwnerCount)));
        }
    }
    owner_index = std::move(loaded);
    return ok();
}

void ChunkTable::clear() {
    owner_index.clear();
}

// ------------------------------------------------------------------------- AdjacencyTable

Status AdjacencyTable::check(std::span<const std::uint32_t> first,
                             std::span<const std::uint32_t> neighbour) {
    if (first.empty()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "adjacency has no row offsets at all"));
    }
    if (first.front() != 0) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("adjacency's first offset is {}, not zero", first.front())));
    }
    for (std::size_t i = 1; i < first.size(); ++i) {
        if (first[i] < first[i - 1]) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("adjacency offsets decrease at cell {}: {} after {}", i - 1,
                                  first[i], first[i - 1])));
        }
    }
    if (first.back() != neighbour.size()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("adjacency's last offset is {} but there are {} neighbours",
                              first.back(), neighbour.size())));
    }
    const std::size_t cells = first.size() - 1;
    for (std::size_t i = 0; i < neighbour.size(); ++i) {
        if (neighbour[i] >= cells) {
            return std::unexpected(Error(
                ErrorCode::MalformedData,
                std::format("adjacency entry {} names cell {} of {}", i, neighbour[i], cells)));
        }
    }
    return ok();
}

Status AdjacencyTable::set(std::vector<std::uint32_t> first, std::vector<std::uint32_t> neighbour) {
    if (auto status = check(first, neighbour); !status) {
        return status;
    }
    m_first = std::move(first);
    m_neighbour = std::move(neighbour);
    Hasher hasher;
    hasher.add(static_cast<std::uint64_t>(m_first.size()));
    hasher.add(column_bytes(m_first));
    hasher.add(column_bytes(m_neighbour));
    m_hash = hasher.value();
    return ok();
}

void AdjacencyTable::hash_into(Hasher& hasher) const {
    hasher.add(m_hash);
}

void AdjacencyTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u64(static_cast<std::uint64_t>(m_first.size()));
    for (const auto v : m_first) {
        writer.write_u32(v);
    }
    writer.write_u64(static_cast<std::uint64_t>(m_neighbour.size()));
    for (const auto v : m_neighbour) {
        writer.write_u32(v);
    }
}

Status AdjacencyTable::read_from(sim::SaveReader& reader) {
    auto first_count = reader.read_count(kMaxCells + 1, 4);
    if (!first_count) {
        return std::unexpected(std::move(first_count).error().context("adjacency.first"));
    }
    std::vector<std::uint32_t> first;
    if (auto s = read_column(reader, *first_count, first, &sim::SaveReader::read_u32); !s) {
        return std::unexpected(std::move(s).error().context("adjacency.first"));
    }
    auto neighbour_count = reader.read_count(kMaxNeighbours, 4);
    if (!neighbour_count) {
        return std::unexpected(std::move(neighbour_count).error().context("adjacency.neighbour"));
    }
    std::vector<std::uint32_t> neighbour;
    if (auto s = read_column(reader, *neighbour_count, neighbour, &sim::SaveReader::read_u32); !s) {
        return std::unexpected(std::move(s).error().context("adjacency.neighbour"));
    }
    return set(std::move(first), std::move(neighbour));
}

void AdjacencyTable::clear() {
    m_first.clear();
    m_neighbour.clear();
    m_hash = 0;
}

// ------------------------------------------------------------------------------ the world

Result<TableIds> add_lab_tables(sim::World& world) {
    TableIds ids;
    auto grid = world.add_table("grid", std::make_unique<GridTable>());
    if (!grid) {
        return std::unexpected(std::move(grid).error());
    }
    ids.grid = *grid;
    auto cells = world.add_table("cells", std::make_unique<CellTable>());
    if (!cells) {
        return std::unexpected(std::move(cells).error());
    }
    ids.cells = *cells;
    auto population = world.add_table("population", std::make_unique<PopulationTable>());
    if (!population) {
        return std::unexpected(std::move(population).error());
    }
    ids.population = *population;
    auto chunks = world.add_table("chunks", std::make_unique<ChunkTable>());
    if (!chunks) {
        return std::unexpected(std::move(chunks).error());
    }
    ids.chunks = *chunks;
    auto adjacency = world.add_table("adjacency", std::make_unique<AdjacencyTable>());
    if (!adjacency) {
        return std::unexpected(std::move(adjacency).error());
    }
    ids.adjacency = *adjacency;
    return ids;
}

GridTable& grid_table(sim::World& world, const TableIds& ids) noexcept {
    return typed_table<GridTable>(world, ids.grid);
}

const GridTable& grid_table(const sim::World& world, const TableIds& ids) noexcept {
    return typed_table<GridTable>(world, ids.grid);
}

CellTable& cell_table(sim::World& world, const TableIds& ids) noexcept {
    return typed_table<CellTable>(world, ids.cells);
}

const CellTable& cell_table(const sim::World& world, const TableIds& ids) noexcept {
    return typed_table<CellTable>(world, ids.cells);
}

PopulationTable& population_table(sim::World& world, const TableIds& ids) noexcept {
    return typed_table<PopulationTable>(world, ids.population);
}

const PopulationTable& population_table(const sim::World& world, const TableIds& ids) noexcept {
    return typed_table<PopulationTable>(world, ids.population);
}

ChunkTable& chunk_table(sim::World& world, const TableIds& ids) noexcept {
    return typed_table<ChunkTable>(world, ids.chunks);
}

const ChunkTable& chunk_table(const sim::World& world, const TableIds& ids) noexcept {
    return typed_table<ChunkTable>(world, ids.chunks);
}

AdjacencyTable& adjacency_table(sim::World& world, const TableIds& ids) noexcept {
    return typed_table<AdjacencyTable>(world, ids.adjacency);
}

const AdjacencyTable& adjacency_table(const sim::World& world, const TableIds& ids) noexcept {
    return typed_table<AdjacencyTable>(world, ids.adjacency);
}

Result<GridLayout> validate_world(const sim::World& world, const TableIds& ids) {
    auto layout = grid_table(world, ids).layout();
    if (!layout) {
        return std::unexpected(std::move(layout).error().context("the grid row"));
    }
    const auto expect = [&](std::string_view table, std::size_t rows,
                            std::size_t wanted) -> Status {
        if (rows != wanted) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("the grid row describes {} {} but the {} table has {} rows",
                                  wanted, table == "chunks" ? "chunks" : "cells", table, rows)));
        }
        return ok();
    };
    if (auto s = expect("cells", cell_table(world, ids).row_count(), layout->cell_count()); !s) {
        return std::unexpected(s.error());
    }
    if (auto s =
            expect("population", population_table(world, ids).row_count(), layout->cell_count());
        !s) {
        return std::unexpected(s.error());
    }
    if (auto s = expect("chunks", chunk_table(world, ids).row_count(), layout->chunk_count()); !s) {
        return std::unexpected(s.error());
    }
    if (auto s = expect("adjacency", adjacency_table(world, ids).row_count(), layout->cell_count());
        !s) {
        return std::unexpected(s.error());
    }
    return *layout;
}

}  // namespace atlas::lab

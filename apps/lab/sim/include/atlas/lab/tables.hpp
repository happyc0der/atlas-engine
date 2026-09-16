// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The five tables of the lab's world. Synthetic names only; nothing here means anything.
///
/// | Table        | Rows | Why it exists                                                     |
/// |--------------|------|-------------------------------------------------------------------|
/// | grid         | 1    | Identity, so a load whose shape disagrees with its rows is caught |
/// | cells        | N    | What is drawn and picked                                          |
/// | population   | N    | A second write target, so batching has more than one system      |
/// | chunks       | C    | A per-chunk value, and what culling displays                      |
/// | adjacency    | N    | The indirection access pattern, and a hostile-input target        |
///
/// Chunk aggregates are deliberately not a table: they are derived, and derived data belongs
/// in the snapshot, never in a world that hashes everything it holds.
///
/// Columns are hashed as whole byte spans rather than element by element. Both walk the same
/// bytes in the same order on a little-endian machine, which every supported target is, and the
/// golden-hash test is what would notice if one were not. At a million rows the difference is
/// a function call per row.
///
/// Thread affinity: written on the simulation thread during commit; read by anyone holding a
/// const World.

#include <atlas/core/result.hpp>
#include <atlas/lab/grid_layout.hpp>
#include <atlas/simulation/table.hpp>
#include <atlas/simulation/world.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace atlas::lab {

inline constexpr std::uint16_t kOwnerCount = 16;
inline constexpr std::uint8_t kColorCount = 8;
inline constexpr std::uint32_t kPopulationCap = 1'000'000;

/// One row: the shape and seed the other tables were generated from.
class GridTable final : public sim::Table {
  public:
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t chunk_size = 0;
    std::uint64_t seed = 0;

    [[nodiscard]] std::size_t row_count() const noexcept override { return 1; }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;

    /// The layout these numbers describe, or why they describe none.
    [[nodiscard]] Result<GridLayout> layout() const;
};

class CellTable final : public sim::Table {
  public:
    static constexpr std::size_t kMaxRows = GridLayout::kMaxCells;

    std::vector<std::uint32_t> region_value;
    std::vector<std::uint16_t> owner_index;
    std::vector<std::uint8_t> color_index;

    void resize(std::size_t rows);

    [[nodiscard]] std::size_t row_count() const noexcept override { return region_value.size(); }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

class PopulationTable final : public sim::Table {
  public:
    static constexpr std::size_t kMaxRows = GridLayout::kMaxCells;

    std::vector<std::uint32_t> population_value;

    void resize(std::size_t rows);

    [[nodiscard]] std::size_t row_count() const noexcept override {
        return population_value.size();
    }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

class ChunkTable final : public sim::Table {
  public:
    static constexpr std::size_t kMaxRows = GridLayout::kMaxCells;

    std::vector<std::uint16_t> owner_index;

    void resize(std::size_t rows);

    [[nodiscard]] std::size_t row_count() const noexcept override { return owner_index.size(); }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

/// Compressed sparse row adjacency: cell i's neighbours are neighbour[first[i] .. first[i+1]).
///
/// Immutable once set, and set() is the only way in. That is what makes hashing it once
/// legitimate: the columns cannot change under the cached hash, so a per-tick walk over what
/// is by far the largest table would prove nothing it does not already know.
class AdjacencyTable final : public sim::Table {
  public:
    static constexpr std::size_t kMaxCells = GridLayout::kMaxCells;
    /// Eight per cell is twice what a four-neighbour grid uses; a save claiming more is not a
    /// grid this application generated.
    static constexpr std::size_t kMaxNeighbours = std::size_t{8} * GridLayout::kMaxCells;

    /// The invariants, checked in this order and each with its own message: first has
    /// cells + 1 entries, first[0] is zero, first never decreases, first.back() equals the
    /// neighbour count, and every neighbour names an existing cell.
    [[nodiscard]] static Status check(std::span<const std::uint32_t> first,
                                      std::span<const std::uint32_t> neighbour);

    /// Failure: whatever check() reports; the table is unchanged.
    [[nodiscard]] Status set(std::vector<std::uint32_t> first,
                             std::vector<std::uint32_t> neighbour);

    [[nodiscard]] std::span<const std::uint32_t> first() const noexcept { return m_first; }

    [[nodiscard]] std::span<const std::uint32_t> neighbour() const noexcept { return m_neighbour; }

    /// Precondition: cell < row_count().
    [[nodiscard]] std::span<const std::uint32_t> neighbours_of(std::uint32_t cell) const noexcept {
        return std::span{m_neighbour}.subspan(m_first[cell], m_first[cell + 1] - m_first[cell]);
    }

    [[nodiscard]] std::size_t row_count() const noexcept override {
        return m_first.empty() ? 0 : m_first.size() - 1;
    }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;

  private:
    std::vector<std::uint32_t> m_first;
    std::vector<std::uint32_t> m_neighbour;
    std::uint64_t m_hash = 0;
};

struct TableIds {
    sim::TableId grid = sim::TableId::Invalid;
    sim::TableId cells = sim::TableId::Invalid;
    sim::TableId population = sim::TableId::Invalid;
    sim::TableId chunks = sim::TableId::Invalid;
    sim::TableId adjacency = sim::TableId::Invalid;
};

/// Add the five tables, empty, to a world. Failure: whatever World::add_table reports.
[[nodiscard]] Result<TableIds> add_lab_tables(sim::World& world);

/// Typed access. Precondition: the id came from add_lab_tables on this world.
[[nodiscard]] GridTable& grid_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const GridTable& grid_table(const sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] CellTable& cell_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const CellTable& cell_table(const sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] PopulationTable& population_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const PopulationTable& population_table(const sim::World& world,
                                                      const TableIds& ids) noexcept;
[[nodiscard]] ChunkTable& chunk_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const ChunkTable& chunk_table(const sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const AdjacencyTable& adjacency_table(const sim::World& world,
                                                    const TableIds& ids) noexcept;
[[nodiscard]] AdjacencyTable& adjacency_table(sim::World& world, const TableIds& ids) noexcept;

/// The cross-table check no single table can do: the grid row describes a layout, and every
/// other table has the row count that layout implies. Run after generation and after every
/// load, because sim::load restores each table on its own and cannot know they belong together.
///
/// Failure: MalformedData naming the table and both counts.
[[nodiscard]] Result<GridLayout> validate_world(const sim::World& world, const TableIds& ids);

}  // namespace atlas::lab

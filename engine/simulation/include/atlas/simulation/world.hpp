// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The authoritative state: every table, and the canonical hash over them.
///
/// The world owns the tables and nothing else. It does not know what they hold, does not
/// advance them, and contains no rules. Advancing is the kernel's job, and the rules are the
/// application's.
///
/// **Ordering is by table identifier, always.** Tables are registered in whatever order the
/// application happens to construct them, and that order must not reach the hash or the save
/// file, or a state would hash differently depending on how it was assembled. See
/// docs/DETERMINISM.md.
///
/// Thread affinity: the compute phase sees this as `const` and the commit phase as mutable.
/// That split is enforced by the type system rather than by convention, which is what makes
/// moving the compute phase onto workers in M8 a scheduling change.

#include <atlas/core/result.hpp>
#include <atlas/simulation/table.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::sim {

/// A table's name and identifier, for reporting.
struct TableInfo {
    TableId id = TableId::Invalid;
    std::string_view name;
    std::size_t row_count = 0;
};

class World {
  public:
    World();
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&& other) noexcept;
    World& operator=(World&& other) noexcept;

    /// Register a table under a name, taking ownership.
    ///
    /// Fails on a duplicate name, and on a name that collides with a different one already
    /// registered. A collision is astronomically unlikely and catastrophic if ignored, since
    /// two tables sharing an identifier would silently overwrite each other in a save file,
    /// so it is checked rather than assumed away.
    [[nodiscard]] Result<TableId> add_table(std::string_view name, std::unique_ptr<Table> table);

    [[nodiscard]] Table* table(TableId id) noexcept;
    [[nodiscard]] const Table* table(TableId id) const noexcept;

    [[nodiscard]] bool contains(TableId id) const noexcept;
    [[nodiscard]] std::size_t table_count() const noexcept;

    /// Every table, ordered by identifier.
    [[nodiscard]] std::vector<TableInfo> tables() const;

    /// Identifiers only, ordered. The order hashing and saving use.
    [[nodiscard]] std::span<const TableId> ids() const noexcept;

    /// The canonical hash of all authoritative state.
    ///
    /// Covers the identifier and contents of every table, in identifier order. The
    /// identifier goes in as well as the contents so that moving a row between two tables
    /// changes the hash even when the bytes are the same.
    [[nodiscard]] std::uint64_t hash() const;

    /// The hash of one table alone, for attributing a divergence to a system's writes.
    [[nodiscard]] Result<std::uint64_t> table_hash(TableId id) const;

    /// Empty every table without deregistering any.
    ///
    /// The tables themselves stay, because the schedule refers to them by identifier and a
    /// load must not change what exists, only what is in it.
    void clear();

  private:
    struct Entry;
    std::vector<Entry> m_entries;  ///< Kept sorted by identifier.
    std::vector<TableId> m_ids;    ///< Parallel to m_entries, for cheap iteration.

    [[nodiscard]] const Entry* find(TableId id) const noexcept;
};

}  // namespace atlas::sim

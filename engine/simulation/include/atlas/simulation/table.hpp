// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Authoritative simulation state, and the little the kernel needs to know about it.
///
/// The kernel does not define what a table holds. Provinces, populations and markets are
/// the application's business; layouts are chosen from measured query patterns, which is the
/// whole point of ADR-0004 keeping this separate from the scene's entity storage.
///
/// But the kernel has to hash state canonically, save it, and load it back, and it cannot do
/// any of that for data it knows nothing about. `Table` is the smallest interface that makes
/// those three possible while leaving the layout entirely to the application: it is asked to
/// hash itself and to write and read itself, and nothing else.
///
/// Virtual dispatch here is per table per tick, not per row, so the cost is noise next to
/// the work inside.
///
/// Thread affinity: a table is read by the compute phase and written only by the commit
/// phase. Until M8 both run on the main thread; the contract is what makes moving the
/// compute phase to workers a scheduling change rather than a redesign.

#include <atlas/core/hash.hpp>
#include <atlas/core/result.hpp>
#include <atlas/simulation/save_stream.hpp>

#include <cstdint>
#include <string_view>

namespace atlas::sim {

/// A table's identity: the hash of its name.
///
/// Derived from the name rather than assigned by registration order, so that a save file
/// written by one build reads correctly in another that registers tables in a different
/// order. Zero is reserved for "no table", so a zeroed structure is not a reference to
/// whichever table happened to be registered first.
enum class TableId : std::uint32_t { Invalid = 0 };

/// The identifier for a table name.
///
/// Uses the canonical hash, whose algorithm identity is versioned, so the mapping from name
/// to identifier is stable across builds and is recorded in save files.
[[nodiscard]] constexpr TableId table_id(std::string_view name) noexcept {
    // Folded from the 64-bit canonical hash rather than a separate 32-bit one, so there is
    // one hash algorithm in the project and one version number governing it.
    const std::uint64_t full = hash_string(name);
    const auto hashed = static_cast<std::uint32_t>((full >> 32) ^ (full & 0xFFFF'FFFFULL));
    // Zero means "no table", so a name that happens to hash to it is nudged. Choosing the
    // next value rather than rejecting the name keeps the mapping total: no name is
    // unusable, and the collision this introduces is with 1, which is no more likely than
    // any other collision in a 32-bit space.
    return TableId{hashed == 0 ? 1U : hashed};
}

[[nodiscard]] constexpr bool valid(TableId id) noexcept {
    return id != TableId::Invalid;
}

/// One table of authoritative state.
///
/// Implemented by the application. The kernel holds these, orders them by identifier, and
/// asks them to hash and serialise themselves; it never looks inside.
class Table {
  public:
    Table() = default;
    virtual ~Table() = default;

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) = delete;
    Table& operator=(Table&&) = delete;

    /// Number of rows, for reporting and for bounds checks on load.
    [[nodiscard]] virtual std::size_t row_count() const noexcept = 0;

    /// Feed every authoritative value into the hasher, in a fixed order.
    ///
    /// The order must depend only on the contents, never on how they were inserted or on
    /// where the container chose to put them. Index order for arrays; a sorted index array
    /// for anything associative. See docs/DETERMINISM.md.
    ///
    /// Derived, cached, or presentation-only values must **not** be fed in: two states that
    /// differ only in a cache are the same state, and hashing the cache would report a
    /// divergence that is not one.
    virtual void hash_into(Hasher& hasher) const = 0;

    /// Write the table in canonical order.
    virtual void write_to(SaveWriter& writer) const = 0;

    /// Read the table back, replacing whatever it held.
    ///
    /// The input is untrusted: it may come from a file someone else wrote. Validate counts
    /// and bounds before allocating or indexing, and return an error rather than trusting a
    /// length. `SaveReader` does the bounds checking for the reads themselves; what it
    /// cannot check is whether a value makes sense to this table.
    [[nodiscard]] virtual Status read_from(SaveReader& reader) = 0;

    /// Discard everything, returning the table to its freshly constructed state.
    ///
    /// Used before a load, so a failed one cannot leave rows from the previous state mixed
    /// with rows from the file.
    virtual void clear() = 0;
};

}  // namespace atlas::sim

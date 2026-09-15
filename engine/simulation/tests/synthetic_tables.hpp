// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Synthetic tables for testing the kernel.
///
/// Deliberately meaningless. A `ValueTable` holds integers called values and an owner index
/// that indexes nothing; they exist to be hashed, saved, loaded and mutated, and carry no
/// domain meaning of any kind. The engine has no game in it and neither do its tests.

#include <atlas/simulation/table.hpp>

#include <cstdint>
#include <vector>

namespace atlas::sim::testing {

/// Rows of two integers each.
class ValueTable final : public Table {
  public:
    static constexpr std::size_t kMaxRows = 1'000'000;

    std::vector<std::int32_t> value;
    std::vector<std::uint16_t> owner_index;

    void resize(std::size_t rows) {
        value.assign(rows, 0);
        owner_index.assign(rows, 0);
    }

    [[nodiscard]] std::size_t row_count() const noexcept override { return value.size(); }

    void hash_into(Hasher& hasher) const override {
        hasher.add(static_cast<std::uint64_t>(value.size()));
        for (std::size_t i = 0; i < value.size(); ++i) {
            hasher.add(value[i]);
            hasher.add(owner_index[i]);
        }
    }

    void write_to(SaveWriter& writer) const override {
        writer.write_u64(static_cast<std::uint64_t>(value.size()));
        for (std::size_t i = 0; i < value.size(); ++i) {
            writer.write_i32(value[i]);
            writer.write_u16(owner_index[i]);
        }
    }

    [[nodiscard]] Status read_from(SaveReader& reader) override {
        // Six bytes per row: a 32-bit value and a 16-bit index. Passing that is what lets the
        // reader refuse a count the file has no data to fill.
        auto rows = reader.read_count(kMaxRows, 6);
        if (!rows) {
            return std::unexpected(std::move(rows).error());
        }

        std::vector<std::int32_t> loaded_value;
        std::vector<std::uint16_t> loaded_owner;
        loaded_value.reserve(*rows);
        loaded_owner.reserve(*rows);

        for (std::size_t i = 0; i < *rows; ++i) {
            auto v = reader.read_i32();
            if (!v) {
                return std::unexpected(std::move(v).error());
            }
            auto o = reader.read_u16();
            if (!o) {
                return std::unexpected(std::move(o).error());
            }
            loaded_value.push_back(*v);
            loaded_owner.push_back(*o);
        }

        // Committed only once every row has been read, so a truncated file leaves the table
        // as it was rather than half-replaced.
        value = std::move(loaded_value);
        owner_index = std::move(loaded_owner);
        return ok();
    }

    void clear() override {
        value.clear();
        owner_index.clear();
    }
};

/// A single counter, for tests that want a second table without a second shape.
class CounterTable final : public Table {
  public:
    std::uint64_t count = 0;

    [[nodiscard]] std::size_t row_count() const noexcept override { return 1; }

    void hash_into(Hasher& hasher) const override { hasher.add(count); }

    void write_to(SaveWriter& writer) const override { writer.write_u64(count); }

    [[nodiscard]] Status read_from(SaveReader& reader) override {
        auto loaded = reader.read_u64();
        if (!loaded) {
            return std::unexpected(std::move(loaded).error());
        }
        count = *loaded;
        return ok();
    }

    void clear() override { count = 0; }
};

}  // namespace atlas::sim::testing

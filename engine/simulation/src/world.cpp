// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/profile.hpp>
#include <atlas/simulation/world.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace atlas::sim {

struct World::Entry {
    TableId id = TableId::Invalid;
    std::string name;
    std::unique_ptr<Table> table;
};

World::World() = default;
World::~World() = default;
World::World(World&& other) noexcept = default;
World& World::operator=(World&& other) noexcept = default;

Result<TableId> World::add_table(std::string_view name, std::unique_ptr<Table> table) {
    if (name.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "a table needs a name"));
    }
    if (table == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, std::format("table '{}' is null", name)));
    }

    const TableId id = table_id(name);

    if (const Entry* existing = find(id); existing != nullptr) {
        if (existing->name == name) {
            return std::unexpected(Error(ErrorCode::AlreadyExists,
                                         std::format("table '{}' is already registered", name)));
        }
        // Two different names sharing an identifier. Vanishingly rare and silent if allowed:
        // the second would shadow the first in every lookup, and a save file would hold one
        // table's bytes under the other's name.
        return std::unexpected(Error(
            ErrorCode::AlreadyExists,
            std::format("table '{}' hashes to the same identifier as '{}'; rename one of them",
                        name, existing->name)));
    }

    // Inserted in identifier order rather than appended, so every walk is canonical without
    // sorting first and without anyone having to remember to.
    const auto at = std::ranges::lower_bound(m_ids, id);
    const auto index = static_cast<std::size_t>(at - m_ids.begin());

    m_ids.insert(at, id);
    m_entries.insert(m_entries.begin() + static_cast<std::ptrdiff_t>(index),
                     Entry{.id = id, .name = std::string{name}, .table = std::move(table)});
    return id;
}

const World::Entry* World::find(TableId id) const noexcept {
    const auto at = std::ranges::lower_bound(m_ids, id);
    if (at == m_ids.end() || *at != id) {
        return nullptr;
    }
    return &m_entries[static_cast<std::size_t>(at - m_ids.begin())];
}

Table* World::table(TableId id) noexcept {
    const Entry* entry = find(id);
    return entry != nullptr ? entry->table.get() : nullptr;
}

const Table* World::table(TableId id) const noexcept {
    const Entry* entry = find(id);
    return entry != nullptr ? entry->table.get() : nullptr;
}

bool World::contains(TableId id) const noexcept {
    return find(id) != nullptr;
}

std::size_t World::table_count() const noexcept {
    return m_entries.size();
}

std::span<const TableId> World::ids() const noexcept {
    return m_ids;
}

std::vector<TableInfo> World::tables() const {
    std::vector<TableInfo> out;
    out.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        out.push_back(TableInfo{
            .id = entry.id,
            .name = entry.name,
            .row_count = entry.table->row_count(),
        });
    }
    return out;
}

std::uint64_t World::hash() const {
    ATLAS_ZONE_NAMED("World::hash");

    Hasher hasher;
    // The count goes in first so that a state with an extra empty table does not hash the
    // same as one without it.
    hasher.add(static_cast<std::uint64_t>(m_entries.size()));

    for (const Entry& entry : m_entries) {
        hasher.add(static_cast<std::uint32_t>(entry.id));
        entry.table->hash_into(hasher);
    }
    return hasher.value();
}

Result<std::uint64_t> World::table_hash(TableId id) const {
    const Entry* entry = find(id);
    if (entry == nullptr) {
        return std::unexpected(
            Error(ErrorCode::NotFound,
                  std::format("no table with identifier {}", static_cast<std::uint32_t>(id))));
    }

    Hasher hasher;
    hasher.add(static_cast<std::uint32_t>(entry->id));
    entry->table->hash_into(hasher);
    return hasher.value();
}

void World::clear() {
    for (Entry& entry : m_entries) {
        entry.table->clear();
    }
}

}  // namespace atlas::sim

// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/profile.hpp>
#include <atlas/simulation/schedule.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace atlas::sim {
namespace {

/// Sort and remove duplicates, so every later comparison is a set operation.
void normalise(std::vector<TableId>& ids) {
    std::ranges::sort(ids);
    const auto extra = std::ranges::unique(ids);
    ids.erase(extra.begin(), extra.end());
}

[[nodiscard]] bool intersects(std::span<const TableId> a, std::span<const TableId> b) noexcept {
    // Both are sorted, so this is a merge rather than a scan of one against the other.
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) {
            ++i;
        } else if (b[j] < a[i]) {
            ++j;
        } else {
            return true;
        }
    }
    return false;
}

/// Whether two systems may run in the same compute batch.
///
/// They may not when one writes a table the other writes, or when one writes a table the
/// other reads. Two readers of the same table are fine: nothing is being changed.
[[nodiscard]] bool conflicts(const System& a, const System& b) noexcept {
    return intersects(a.writes, b.writes) || intersects(a.writes, b.reads) ||
           intersects(a.reads, b.writes);
}

}  // namespace

Schedule::Schedule() = default;
Schedule::~Schedule() = default;
Schedule::Schedule(Schedule&& other) noexcept = default;
Schedule& Schedule::operator=(Schedule&& other) noexcept = default;

Status Schedule::add(SystemDesc desc) {
    if (desc.name.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "a system needs a name"));
    }
    if (desc.compute == nullptr && desc.commit == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("system '{}' has neither a compute nor a commit phase, so adding "
                              "it would only cost time",
                              desc.name)));
    }

    const SystemId id = system_id(desc.name);

    for (const System& existing : m_systems) {
        if (existing.id != id) {
            continue;
        }
        if (existing.name == desc.name) {
            return std::unexpected(
                Error(ErrorCode::AlreadyExists,
                      std::format("system '{}' is already in the schedule", desc.name)));
        }
        return std::unexpected(Error(
            ErrorCode::AlreadyExists,
            std::format("system '{}' hashes to the same identifier as '{}'; rename one of them",
                        desc.name, existing.name)));
    }

    System system;
    system.id = id;
    system.name = std::string{desc.name};
    system.reads = std::move(desc.reads);
    system.writes = std::move(desc.writes);
    system.compute = std::move(desc.compute);
    system.commit = std::move(desc.commit);

    normalise(system.reads);
    normalise(system.writes);

    if (intersects(system.reads, system.writes)) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("system '{}' declares the same table as both read and written, which "
                              "leaves its own ordering ambiguous: compute would see the value from "
                              "before its own commit",
                              system.name)));
    }

    if (system.commit != nullptr && system.writes.empty()) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("system '{}' has a commit phase but declares no writes; the "
                              "conflict analysis would let it run beside anything",
                              system.name)));
    }

    m_systems.push_back(std::move(system));
    m_finalised = false;
    m_batches.clear();
    return ok();
}

Status Schedule::finalise(const World& world) {
    ATLAS_ZONE_NAMED("Schedule::finalise");

    // A table name that does not exist would otherwise conflict with nothing, and the system
    // declaring it would be scheduled alongside anything at all.
    for (const System& system : m_systems) {
        for (const auto& [set, what] : {std::pair{std::cref(system.reads), "reads"},
                                        std::pair{std::cref(system.writes), "writes"}}) {
            for (const TableId id : set.get()) {
                if (!world.contains(id)) {
                    return std::unexpected(
                        Error(ErrorCode::NotFound,
                              std::format("system '{}' {} table {}, which the world does not have",
                                          system.name, what, static_cast<std::uint32_t>(id))));
                }
            }
        }
    }

    // Greedy in declared order: a system joins the open batch when it conflicts with nothing
    // already in it, and otherwise starts a new one. Following the declared order is what
    // makes the grouping reproducible; a cleverer packing could produce fewer batches and
    // would have to be stable against unrelated edits to be worth it.
    m_batches.clear();
    std::vector<std::size_t> open;

    for (std::size_t i = 0; i < m_systems.size(); ++i) {
        const bool fits = std::ranges::none_of(open, [this, i](std::size_t member) {
            return conflicts(m_systems[member], m_systems[i]);
        });

        if (!fits && !open.empty()) {
            m_batches.push_back(Batch{.systems = std::move(open)});
            open.clear();
        }
        open.push_back(i);
    }

    if (!open.empty()) {
        m_batches.push_back(Batch{.systems = std::move(open)});
    }

    m_finalised = true;
    return ok();
}

const System* Schedule::find(SystemId id) const noexcept {
    const auto at = std::ranges::find(m_systems, id, &System::id);
    return at == m_systems.end() ? nullptr : &*at;
}

}  // namespace atlas::sim

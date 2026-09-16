// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Systems, what they touch, and the order they run in.
///
/// A system is a named pair of functions over the world, with a declaration of which tables
/// it reads and which it writes. The kernel knows nothing about what any of them do.
///
/// **Two phases, separated by const.** The compute phase is handed a `const World` and may
/// only write to storage the system owns. The commit phase is handed a mutable one and
/// applies what compute produced. The compiler enforces the split, so a system physically
/// cannot mutate shared state while others are reading it. Until M8 both phases run on the
/// main thread; the separation is what makes moving compute onto workers a scheduling change
/// rather than a redesign.
///
/// **Batches are derived now and executed sequentially.** Systems that touch no common table
/// in a conflicting way are grouped, in declared order. M6 runs the batches one system at a
/// time anyway, but they are computed and validated here so that M8 has nothing left to
/// design. Deriving them also catches a declaration that is wrong today, long before it
/// becomes a data race.
///
/// **Declarations are checked against reality only as far as they can be.** Nothing stops a
/// system writing a table it did not declare, because the kernel hands it the world and
/// cannot see inside. What the kernel can do is notice when the declared sets are
/// inconsistent, and it does. A table that is written must be declared written.
///
/// Thread affinity: building a schedule is main-thread setup. Running one is the kernel's
/// business.

#include <atlas/core/result.hpp>
#include <atlas/simulation/rng.hpp>
#include <atlas/simulation/world.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::sim {

/// A system's identity: the hash of its name.
enum class SystemId : std::uint32_t { Invalid = 0 };

[[nodiscard]] constexpr SystemId system_id(std::string_view name) noexcept {
    const std::uint64_t full = hash_string(name);
    const auto folded = static_cast<std::uint32_t>((full >> 32U) ^ (full & 0xFFFF'FFFFULL));
    return SystemId{folded == 0 ? 1U : folded};
}

[[nodiscard]] constexpr bool valid(SystemId id) noexcept {
    return id != SystemId::Invalid;
}

/// What the compute phase may see.
///
/// The world is const, which is the entire mechanism: a system cannot write shared state
/// here even by mistake. Anything it produces goes into storage the system itself owns.
struct ComputeContext {
    // A reference is the point. This is a parameter aggregate built for one call and never
    // stored, and the const is what stops a system writing shared state during compute.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const World& world;
    Tick tick = 0;
    RngStreams rng;
};

/// What the commit phase may see.
///
/// Mutable, and run one system at a time in declared order, so two systems writing the same
/// table produce the same result every run regardless of how compute was scheduled.
struct CommitContext {
    // As above: a parameter aggregate built for one call and never stored.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    World& world;
    Tick tick;
};

/// One system's declaration.
struct SystemDesc {
    std::string_view name;

    /// Tables read during compute that some *other* system writes. A system's own written
    /// tables are not listed here: add() refuses a table in both sets, because compute would
    /// see the value from before the system's own commit and the ordering would be
    /// ambiguous. A system may read what it writes without declaring it. Reading an
    /// undeclared table is not detectable by the kernel; declaring it is what makes the
    /// conflict analysis mean anything.
    std::vector<TableId> reads;

    /// Tables written during commit.
    std::vector<TableId> writes;

    /// Runs with a const world. May be empty for a system that only commits.
    std::function<void(const ComputeContext&)> compute;

    /// Applies what compute produced. May be empty for a system that only reads.
    std::function<void(const CommitContext&)> commit;
};

/// A group of systems with no conflicting access between them.
struct Batch {
    /// Indices into the schedule's system list, ascending.
    std::vector<std::size_t> systems;
};

/// One system as the schedule holds it.
struct System {
    SystemId id = SystemId::Invalid;
    std::string name;
    std::vector<TableId> reads;   ///< Sorted and deduplicated.
    std::vector<TableId> writes;  ///< Sorted and deduplicated.
    std::function<void(const ComputeContext&)> compute;
    std::function<void(const CommitContext&)> commit;
};

class Schedule {
  public:
    Schedule();
    ~Schedule();

    Schedule(const Schedule&) = delete;
    Schedule& operator=(const Schedule&) = delete;
    Schedule(Schedule&& other) noexcept;
    Schedule& operator=(Schedule&& other) noexcept;

    /// Add a system. Order of addition is the execution order of the commit phase.
    ///
    /// Fails on a duplicate name, on a name colliding with a different one, on a table that
    /// is both read and written by the same system, and on a system that does nothing.
    /// A table in both sets is refused because it makes the system's own ordering ambiguous:
    /// compute would see the value from before its own commit, which is almost never what
    /// the author meant and is impossible to notice from the outside.
    [[nodiscard]] Status add(SystemDesc desc);

    /// Check the declarations against the world and derive the batches.
    ///
    /// Must be called before the schedule is run. Fails if a system declares a table the
    /// world does not have, because a typo in a table name would otherwise silently mean
    /// "conflicts with nothing" and the system would be scheduled alongside anything.
    [[nodiscard]] Status finalise(const World& world);

    [[nodiscard]] bool finalised() const noexcept { return m_finalised; }

    [[nodiscard]] std::span<const System> systems() const noexcept { return m_systems; }

    [[nodiscard]] std::span<const Batch> batches() const noexcept { return m_batches; }

    [[nodiscard]] std::size_t size() const noexcept { return m_systems.size(); }

    /// Find a system by identity, or nullptr.
    [[nodiscard]] const System* find(SystemId id) const noexcept;

  private:
    std::vector<System> m_systems;
    std::vector<Batch> m_batches;
    bool m_finalised = false;
};

}  // namespace atlas::sim

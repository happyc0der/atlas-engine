// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/simulation/kernel.hpp>

#include <format>

namespace atlas::sim {
namespace {

constexpr log::Category kSim{"sim"};

}  // namespace

Kernel::Kernel(World& world, Schedule& schedule, CommandQueue& commands, KernelConfig config)
    : m_world(&world), m_schedule(&schedule), m_commands(&commands), m_config(config) {}

Result<TickReport> Kernel::step() {
    ATLAS_ZONE_NAMED("Kernel::step");
    ATLAS_ASSERT_MAIN_THREAD();

    if (!m_schedule->finalised()) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  "the schedule has not been finalised; call finalise() so the declarations are "
                  "checked and the batches derived before anything runs"));
    }

    TickReport report;
    report.tick = m_tick;

    // 1. Commands. Everything from outside enters here and nowhere else, which is what makes
    //    a recording of them a complete recording of what happened.
    {
        ATLAS_ZONE_NAMED("tick commands");
        const std::vector<Command> due = m_commands->drain(m_tick);

        for (const Command& command : due) {
            if (command.target < m_tick) {
                // Too late to be applied at the tick it named. Applying it now would make
                // the result depend on when it arrived, which is the thing being avoided.
                ++m_late_commands;
                ++report.commands_late;
                ATLAS_LOG_WARN(kSim,
                               "dropping a command from source {} stamped for tick {}, which "
                               "is already past at tick {}",
                               static_cast<std::uint32_t>(command.source), command.target, m_tick);
                continue;
            }

            if (auto status = m_commands->apply(*m_world, command); !status) {
                // One bad command from one source must not halt a simulation that others are
                // also driving, so this is counted and skipped rather than returned.
                ++m_invalid_commands;
                ++report.commands_invalid;
                ATLAS_LOG_WARN(kSim, "dropping a command at tick {}: {}", m_tick, status.error());
                continue;
            }
            ++report.commands_applied;
            if (m_config.record_applied_commands) {
                report.applied_commands.push_back(command);
            }
        }
    }

    // 2. Compute. The world is const here, so a system cannot write shared state even by
    //    mistake; whatever it produces goes into storage it owns.
    {
        ATLAS_ZONE_NAMED("tick compute");
        const ComputeContext context{
            .world = *m_world,
            .tick = m_tick,
            .rng = RngStreams{m_config.seed, m_tick},
            .pool = m_config.pool,
        };

        // Walked batch by batch even though every system runs sequentially. The batches are
        // the unit M8 will hand to workers, and walking them now means that change is a
        // scheduling one rather than a rewrite.
        // A batch is a set of systems that write nothing in common, which is what makes running
        // them together safe. Each still writes only its own scratch and the world is const
        // here, so the answer does not depend on the order they finish in.
        //
        // A batch of one runs directly rather than through the pool: waking a thread costs more
        // than most systems do, and the schedule produces batches of one more often than not.
        const auto run_one = [this, &context](std::size_t index) {
            const System& system = m_schedule->systems()[index];
            if (system.compute != nullptr) {
                ATLAS_ZONE_NAMED("system compute");
                system.compute(context);
            }
        };

        for (const Batch& batch : m_schedule->batches()) {
            if (m_config.pool != nullptr && batch.systems.size() > 1) {
                m_config.pool->parallel_for(batch.systems.size(), 1,
                                            [&batch, &run_one](std::size_t begin, std::size_t end) {
                                                for (std::size_t i = begin; i < end; ++i) {
                                                    run_one(batch.systems[i]);
                                                }
                                            });
            } else {
                for (const std::size_t index : batch.systems) {
                    run_one(index);
                }
            }
        }
    }

    // 3. Commit. One system at a time in declared order, so two systems writing the same
    //    table produce the same result whatever order compute ran in.
    {
        ATLAS_ZONE_NAMED("tick commit");
        const CommitContext context{.world = *m_world, .tick = m_tick};

        for (const System& system : m_schedule->systems()) {
            if (system.commit != nullptr) {
                ATLAS_ZONE_NAMED("system commit");
                system.commit(context);
            }
        }
    }

    // 4. Hash, after everything has been applied.
    {
        ATLAS_ZONE_NAMED("tick hash");
        report.state_hash = m_world->hash();

        if (m_config.record_system_hashes) {
            for (const System& system : m_schedule->systems()) {
                if (system.writes.empty()) {
                    continue;
                }
                // A system's sub-hash covers the tables it declared it writes. That is what
                // makes a divergence attributable: the first system whose sub-hash differs
                // is the first one that produced a different result.
                Hasher hasher;
                for (const TableId id : system.writes) {
                    auto hashed = m_world->table_hash(id);
                    if (!hashed) {
                        return std::unexpected(std::move(hashed).error().context(
                            std::format("hashing the writes of system '{}'", system.name)));
                    }
                    hasher.add(*hashed);
                }
                report.system_hashes.push_back(
                    SystemHash{.system = system.id, .hash = hasher.value()});
            }
        }
    }

    ++m_tick;
    return report;
}

Result<std::vector<TickReport>> Kernel::run(std::uint64_t count) {
    std::vector<TickReport> reports;
    reports.reserve(static_cast<std::size_t>(count));

    for (std::uint64_t i = 0; i < count; ++i) {
        auto report = step();
        if (!report) {
            return std::unexpected(std::move(report).error());
        }
        reports.push_back(std::move(*report));
    }
    return reports;
}

}  // namespace atlas::sim

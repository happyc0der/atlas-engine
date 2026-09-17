// SPDX-License-Identifier: GPL-3.0-or-later
//
// The measurement path for the simulation: the lab's world, driven by the kernel, with no
// device anywhere near it. atlas_lab --headless is the acceptance path and prints an
// observation; this is where numbers come from.
//
// Scenarios, as docs/PERFORMANCE.md names them for M7:
//   simulation/tick            a full kernel tick at 10k, 100k and 1M cells
//   simulation/system/<name>   each system's compute+commit alone at 1M cells, so a tick's
//                              cost can be attributed rather than guessed at
//   simulation/hash            the world hash alone at 1M cells, since a tick pays it too
//   simulation/run             deterministic runs of 1k, 10k and 100k ticks on a small grid
//   simulation/snapshot        building the snapshot at 100k and 1M cells

#include <atlas/core/assert.hpp>
#include <atlas/lab/commands.hpp>
#include <atlas/lab/generate.hpp>
#include <atlas/lab/snapshot.hpp>
#include <atlas/lab/systems.hpp>
#include <atlas/simulation/kernel.hpp>

#include "harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace {

using atlas::bench::Result;

struct Lab {
    atlas::lab::LabWorld world;
    atlas::sim::Schedule schedule;
    atlas::sim::CommandQueue commands;
    std::shared_ptr<atlas::lab::CellBound> bound = std::make_shared<atlas::lab::CellBound>(0);
};

/// A benchmark that cannot set itself up must say so and stop, never measure the setup's
/// absence. Checked explicitly: an assertion is compiled out of the build being measured,
/// and an expression inside one is never evaluated there.
void require(const atlas::Status& status, const char* what) {
    if (!status) {
        std::fprintf(stderr, "bench_simulation: %s: %s\n", what,
                     status.error().to_string().c_str());
        std::abort();
    }
}

[[nodiscard]] std::unique_ptr<Lab> make_lab(std::uint32_t side, std::uint32_t chunk) {
    auto lab = std::make_unique<Lab>();
    auto generated =
        atlas::lab::generate({.width = side, .height = side, .chunk_size = chunk, .seed = 11});
    if (!generated) {
        require(std::unexpected(generated.error()), "generating the world");
    }
    lab->world = std::move(*generated);
    lab->bound->store(lab->world.layout.cell_count());
    require(atlas::lab::add_lab_systems(lab->schedule, lab->world.ids), "adding systems");
    require(lab->schedule.finalise(lab->world.world), "finalising the schedule");
    require(atlas::lab::register_lab_commands(lab->commands, lab->world.ids, lab->bound),
            "registering commands");
    return lab;
}

/// A step that fails would be measured as fast. Every step's result is checked.
void step_or_die(atlas::sim::Kernel& kernel) {
    const auto report = kernel.step();
    if (!report) {
        require(std::unexpected(report.error()), "stepping the kernel");
    }
}

[[nodiscard]] Result with_units(Result result, std::uint64_t units, std::string unit) {
    result.units_per_iteration = units;
    result.unit_name = std::move(unit);
    return result;
}

std::vector<Result> run() {
    atlas::mark_main_thread();
    std::vector<Result> results;

    // A full tick, three sizes. Iterations scale down with size so each point takes a
    // similar wall time; the warm-up absorbs the first tick's scratch allocation.
    for (const auto [side, chunk, iterations] :
         {std::tuple{100U, 10U, 200U}, std::tuple{320U, 32U, 60U}, std::tuple{1024U, 32U, 12U}}) {
        auto lab = make_lab(side, chunk);
        atlas::sim::Kernel kernel(lab->world.world, lab->schedule, lab->commands,
                                  {.seed = 11, .record_system_hashes = false});
        const std::uint64_t cells = lab->world.layout.cell_count();
        results.push_back(
            with_units(atlas::bench::measure("simulation/tick", std::format("cells={}", cells),
                                             iterations, 2, [&] { step_or_die(kernel); }),
                       cells, "cells"));
    }

    // Attribution at a million cells: each system alone, and the hash alone.
    {
        auto lab = make_lab(1024, 32);
        const std::uint64_t cells = lab->world.layout.cell_count();
        const atlas::sim::RngStreams rng(11, 0);
        for (const auto& system : lab->schedule.systems()) {
            const atlas::sim::ComputeContext compute{
                .world = lab->world.world, .tick = 1, .rng = rng};
            const atlas::sim::CommitContext commit{.world = lab->world.world, .tick = 1};
            std::string name = "simulation/system/";
            for (const char c : system.name) {
                name.push_back(c == ' ' ? '_' : c);
            }
            results.push_back(
                with_units(atlas::bench::measure(name, std::format("cells={}", cells), 12, 2,
                                                 [&] {
                                                     system.compute(compute);
                                                     system.commit(commit);
                                                 }),
                           cells, "cells"));
        }
        results.push_back(
            with_units(atlas::bench::measure("simulation/hash", std::format("cells={}", cells), 12,
                                             2, [&] { (void)lab->world.world.hash(); }),
                       cells, "cells"));

        // Decomposed per table, which is what the "hash only what a tick wrote" idea needs in
        // order to be answerable: if the tables a tick writes are the expensive ones, the idea
        // saves nothing here, whatever it might save in some other workload.
        for (const auto& info : lab->world.world.tables()) {
            results.push_back(with_units(
                atlas::bench::measure("simulation/hash/table",
                                      std::format("{} cells={}", info.name, cells), 12, 2,
                                      [&] { (void)lab->world.world.table_hash(info.id); }),
                cells, "cells"));
        }
    }

    // Deterministic runs: many ticks on a grid small enough that the tick loop, not the
    // per-cell work, is what is measured. Two commands per tick, as the golden test does.
    for (const std::uint64_t ticks : {1'000ULL, 10'000ULL, 100'000ULL}) {
        results.push_back(with_units(
            atlas::bench::measure("simulation/run", std::format("ticks={} cells=256", ticks),
                                  ticks >= 100'000 ? 1 : 3, 1,
                                  [&] {
                                      auto lab = make_lab(16, 4);
                                      atlas::sim::Kernel kernel(
                                          lab->world.world, lab->schedule, lab->commands,
                                          {.seed = 11, .record_system_hashes = false});
                                      for (std::uint64_t i = 0; i < ticks; ++i) {
                                          require(atlas::lab::submit_synthetic_commands(
                                                      lab->commands, kernel.current_tick(), 2, 11,
                                                      lab->world.layout.cell_count()),
                                                  "submitting commands");
                                          step_or_die(kernel);
                                      }
                                  }),
            ticks, "ticks"));
    }

    // The snapshot, which is what a frame pays after the last tick.
    for (const auto [side, chunk] : {std::pair{320U, 32U}, std::pair{1024U, 32U}}) {
        auto lab = make_lab(side, chunk);
        const std::uint64_t cells = lab->world.layout.cell_count();
        atlas::lab::CellSnapshot snapshot;
        results.push_back(with_units(
            atlas::bench::measure("simulation/snapshot", std::format("cells={}", cells), 30, 3,
                                  [&] {
                                      atlas::lab::fill_snapshot(snapshot, lab->world.world,
                                                                lab->world.ids, lab->world.layout,
                                                                {});
                                  }),
            cells, "cells"));

        // The same work, but into a snapshot allocated afresh every time, which is what
        // publishing does today. The difference between the two is what a pool would save, and
        // docs/DEFERRED.md made measuring it the condition for building one.
        std::shared_ptr<const atlas::lab::CellSnapshot> published;
        results.push_back(with_units(
            atlas::bench::measure(
                "simulation/snapshot_fresh", std::format("cells={}", cells), 30, 3,
                [&] {
                    auto fresh = atlas::lab::build_snapshot(lab->world.world, lab->world.ids,
                                                            lab->world.layout, {});
                    require(fresh != nullptr
                                ? atlas::ok()
                                : std::unexpected(atlas::Error(atlas::ErrorCode::Internal,
                                                               "build_snapshot returned nothing")),
                            "building a snapshot");
                    // Held while the next is built, which is what the channel does: the
                    // allocator cannot hand back the block it just freed, and if allocation
                    // were the cost this is where it would show.
                    published = std::move(fresh);
                }),
            cells, "cells"));
    }
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("simulation", run);

}  // namespace

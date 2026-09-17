// SPDX-License-Identifier: GPL-3.0-or-later
//
// M8's exit criterion, in the place it has to hold: the same simulation, run with different
// numbers of workers, must produce the same state.
//
// The grid here is larger than the golden scenario's on purpose. A system only splits itself
// when it has more rows than one chunk's worth, so a small world would run everything on the
// calling thread and this would prove nothing while appearing to pass.

#include <atlas/tasks/worker_pool.hpp>

#include "lab_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

using atlas::lab::testing::LabHarness;
using atlas::tasks::WorkerPool;

namespace {

/// Big enough that both row-splitting systems cross their grain, small enough to stay quick.
constexpr atlas::lab::LabConfig kWide{.width = 256, .height = 256, .chunk_size = 32, .seed = 31};
constexpr std::uint64_t kSeed = 0x0A71'A5'0000'0011ULL;
constexpr std::uint64_t kTicks = 20;

struct Outcome {
    std::uint64_t final_state = 0;
    std::uint64_t over_time = 0;
};

/// `workers` of nothing means no pool at all, which is a different path from a pool with no
/// workers: without one the systems never call parallel_for, and with one they always do and it
/// runs inline. Comparing against the first catches a mistake inside the split; comparing the
/// rest against each other catches a mistake that depends on the worker count. A first version
/// of this used a pool of zero as its baseline and could detect only the second — a deliberately
/// dropped row went unnoticed, because both sides dropped it.
[[nodiscard]] Outcome run_with(std::optional<std::size_t> workers) {
    std::optional<WorkerPool> pool;
    if (workers.has_value()) {
        pool = WorkerPool::create(*workers).value();
    }
    LabHarness h(kWide);
    auto kernel = h.kernel(kSeed, pool.has_value() ? &*pool : nullptr);

    atlas::Hasher over_time;
    Outcome outcome;
    for (std::uint64_t i = 0; i < kTicks; ++i) {
        REQUIRE(atlas::lab::submit_synthetic_commands(h.commands, kernel.current_tick(), 3, kSeed,
                                                      h.lab.layout.cell_count())
                    .has_value());
        const auto report = kernel.step();
        REQUIRE(report.has_value());
        over_time.add(report->state_hash);
        outcome.final_state = report->state_hash;
    }
    outcome.over_time = over_time.value();
    return outcome;
}

[[nodiscard]] std::vector<std::size_t> worker_counts() {
    std::vector<std::size_t> counts{1, 2, 4};
    const std::size_t machine = WorkerPool::default_worker_count();
    if (machine > 4) {
        counts.push_back(machine);
    }
    return counts;
}

}  // namespace

TEST_CASE("splitting the work does not change the answer", "[lab][determinism][parallel]") {
    // The baseline is no pool: the systems run their rows in one unsplit pass. Everything else
    // is compared against that, so a mistake inside the split shows up here rather than being
    // shared by every run and cancelling out.
    const Outcome unsplit = run_with(std::nullopt);
    CHECK(unsplit.final_state != 0);

    for (const std::size_t workers : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
        INFO("workers " << workers);
        const Outcome split = run_with(workers);
        CHECK(split.final_state == unsplit.final_state);
        CHECK(split.over_time == unsplit.over_time);
    }
}

TEST_CASE("the simulation produces the same state at every worker count",
          "[lab][determinism][parallel]") {
    const Outcome unsplit = run_with(std::nullopt);
    for (const std::size_t workers : worker_counts()) {
        INFO("workers " << workers);
        const Outcome parallel = run_with(workers);
        CHECK(parallel.final_state == unsplit.final_state);
        CHECK(parallel.over_time == unsplit.over_time);
    }
}

TEST_CASE("the same worker count twice gives the same state", "[lab][determinism][parallel]") {
    // Separates "independent of the worker count" from "reproducible at all": a race that
    // happened to be stable across counts but varied run to run would pass the test above.
    const Outcome first = run_with(std::size_t{4});
    const Outcome second = run_with(std::size_t{4});
    CHECK(first.final_state == second.final_state);
    CHECK(first.over_time == second.over_time);
}

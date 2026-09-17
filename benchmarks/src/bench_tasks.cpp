// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first parallel benchmark, which docs/DEFERRED.md made the condition for creating the
// worker pool at all. Two scenarios, both named in docs/PERFORMANCE.md since M0:
//
//   tasks/dispatch  — what a parallel_for costs when the work is nothing. This is the floor
//                     under every use of the pool: below it, parallelism cannot pay.
//   tasks/scaling   — a fixed amount of work at several worker counts, which says how much of
//                     the machine a parallel loop can actually reach.
//
// Scaling is measured at two work densities, because the first attempt measured the machine's
// memory bandwidth and reported it as the pool's ceiling. One pass of arithmetic over eight
// megabytes reads and writes sixteen of them per call: at four workers that is already around
// 250 GB/s, which is about all this machine has, so adding workers made it slower. Sixteen
// passes over the same data is compute-bound and says what the pool itself can reach. Real work
// sits between the two, and which side it sits on is the useful thing to know about it.

#include <atlas/core/assert.hpp>
#include <atlas/tasks/worker_pool.hpp>

#include "harness.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace {

using atlas::bench::Result;
using atlas::tasks::WorkerPool;

constexpr std::size_t kWorkItems = 1U << 20U;

[[nodiscard]] Result with_units(Result result, std::uint64_t units, std::string unit) {
    result.units_per_iteration = units;
    result.unit_name = std::move(unit);
    return result;
}

[[nodiscard]] WorkerPool make_pool(std::size_t workers) {
    auto pool = WorkerPool::create(workers);
    if (!pool) {
        std::fprintf(stderr, "bench_tasks: %s\n", pool.error().to_string().c_str());
        std::abort();
    }
    return std::move(*pool);
}

/// Enough arithmetic per item that a chunk is worth handing to a thread, and no more.
[[nodiscard]] std::uint64_t mix(std::uint64_t value, std::size_t rounds) noexcept {
    for (std::size_t i = 0; i < rounds; ++i) {
        value ^= value >> 33U;
        value *= 0xFF51'AFD7'ED55'8CCDULL;
        value ^= value >> 29U;
    }
    return value;
}

std::vector<Result> run() {
    atlas::mark_main_thread();
    std::vector<Result> results;

    // What dispatch costs with nothing to do. Reported per call, not per chunk, because what a
    // caller decides is whether to make the call at all.
    for (const std::size_t chunks :
         {std::size_t{1}, std::size_t{8}, std::size_t{64}, std::size_t{1024}}) {
        auto pool = make_pool(WorkerPool::default_worker_count());
        const std::size_t count = chunks;
        results.push_back(atlas::bench::measure(
            "tasks/dispatch", std::format("chunks={} workers={}", chunks, pool.worker_count()),
            2000, 200,
            [&pool, count] { pool.parallel_for(count, 1, [](std::size_t, std::size_t) {}); }));
    }

    // The same work, spread over more and more of the machine. Zero workers is the honest
    // baseline: it is the same code path with the same chunking, run entirely by the caller.
    std::vector<std::uint64_t> data(kWorkItems);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = i;
    }

    std::vector<std::size_t> counts{0, 1, 2, 4};
    const std::size_t machine = WorkerPool::default_worker_count();
    if (machine > 4) {
        counts.push_back(machine);
    }
    for (const auto& density : {std::pair<const char*, std::size_t>{"memory-bound", 1},
                                std::pair<const char*, std::size_t>{"compute-bound", 16}}) {
        for (const std::size_t workers : counts) {
            const std::size_t rounds = density.second;
            auto pool = make_pool(workers);
            results.push_back(with_units(
                atlas::bench::measure(
                    "tasks/scaling", std::format("{} workers={}", density.first, workers),
                    rounds == 1 ? 60 : 20, 5,
                    [&pool, &data, rounds] {
                        pool.parallel_for(data.size(), 4096,
                                          [&data, rounds](std::size_t begin, std::size_t end) {
                                              for (std::size_t i = begin; i < end; ++i) {
                                                  data[i] = mix(data[i], rounds);
                                              }
                                          });
                    }),
                kWorkItems, "items"));
        }
    }
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("tasks", run);

}  // namespace

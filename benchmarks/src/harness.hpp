// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The benchmark harness.
///
/// Reports median and tail percentiles rather than a mean, because a mean hides the stalls
/// that make an application feel bad, and records the machine, compiler and build type
/// alongside every result: a number without them cannot be compared to anything.
///
/// See docs/PERFORMANCE.md for the policy this implements.

#include <atlas/core/time.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::bench {

/// What a result's numbers are measuring.
enum class Metric {
    /// Durations, reported in milliseconds.
    Nanoseconds,
    /// A plain count, such as allocations per frame. Reporting one as a duration would print
    /// "0.000ms" for eight allocations, which is worse than saying nothing.
    Count,
};

/// One measured scenario.
struct Result {
    std::string name;
    /// What was varied, such as the number of quads. Reported alongside the timings so that
    /// two runs are only compared when they measured the same thing.
    std::string parameters;

    std::uint64_t iterations = 0;
    std::uint64_t warmup_iterations = 0;

    std::uint64_t median_ns = 0;
    std::uint64_t p90_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;

    /// Optional unit count per iteration, such as quads drawn, so that throughput can be
    /// reported as well as latency.
    std::uint64_t units_per_iteration = 0;
    std::string unit_name;

    Metric metric = Metric::Nanoseconds;
    /// What the numbers count, when the metric is not a duration.
    std::string count_name;
};

/// Machine and build identity. A result without this cannot be compared to anything.
struct Environment {
    std::string machine;
    std::string cpu;
    std::string cores;
    std::string os;
    std::string compiler;
    std::string build_type;
    std::string commit;
    bool dirty = false;
    bool sanitizer = false;
};

[[nodiscard]] Environment describe_environment();

/// Time `body` repeatedly and summarise.
///
/// Warm-up iterations are discarded: the first run of anything pays for cold caches, lazy
/// page faults and one-off allocations, and including them would measure startup rather
/// than steady state.
[[nodiscard]] Result measure(std::string_view name, std::string_view parameters,
                             std::uint64_t iterations, std::uint64_t warmup,
                             const std::function<void()>& body);

/// Time `body`, which reports its own duration in nanoseconds.
///
/// For a measurement that must exclude part of what it has to do. The renderer benchmark
/// needs this: acquiring a swapchain image waits for the display, and presenting hands the
/// frame to a compositor, so timing either would measure the display's refresh interval
/// rather than the engine. A body returning zero is treated as a skipped iteration.
[[nodiscard]] Result measure_reported(std::string_view name, std::string_view parameters,
                                      std::uint64_t iterations, std::uint64_t warmup,
                                      const std::function<std::uint64_t()>& body);

/// A registered benchmark.
struct Benchmark {
    std::string_view name;
    std::function<std::vector<Result>()> run;
};

/// Every benchmark, in registration order.
[[nodiscard]] std::vector<Benchmark>& registry();

/// Register at startup. Returns a value so it can initialise a namespace-scope constant.
bool register_benchmark(std::string_view name, std::function<std::vector<Result>()> run);

/// Write results as JSON, for comparison against a baseline.
void write_json(const Environment& environment, const std::vector<Result>& results,
                std::string_view path);

/// Write results as a table, for a person.
void write_table(const Environment& environment, const std::vector<Result>& results);

}  // namespace atlas::bench

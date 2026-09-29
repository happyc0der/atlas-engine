// SPDX-License-Identifier: GPL-3.0-or-later
//
// The machine under the engine: how fast it moves memory, how long one miss takes, and how fast
// one core is, with no engine code in any of it.
//
// M28 compared the engine on two machines and read a slow machine as a slow engine until a loop
// written by hand showed the laptop streaming memory thirty times slower than the M4 Pro. These
// rows are that loop, kept, so that a comparison across machines carries its own explanation.
// See docs/PERFORMANCE.md, M29.
#include "harness.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// The largest cycle an index of the visiting order can describe.
constexpr std::size_t kMaxNodes = std::numeric_limits<std::uint32_t>::max();

using atlas::bench::measure;
using atlas::bench::Result;

/// About this many units in every iteration, so that no row is a handful of timer ticks: a
/// small working set repeats its pass until it has done as much as a large one.
constexpr std::uint64_t kUnitsPerIteration = std::uint64_t{1} << 20;

void die(const std::string& why) {
    std::fprintf(stderr, "bench_machine: %s\n", why.c_str());
    std::abort();
}

/// Everything written before this point is written, and nothing the compiler knows about memory
/// survives it. Without it a pass that writes what the previous pass wrote is a dead store the
/// optimiser may delete, and the row would report a machine that does no work.
void clobber_memory() {
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

/// A value the compiler must treat as unknown, so that it cannot fold arithmetic on it.
template <typename T> [[nodiscard]] T opaque(T value) {
    const volatile T copy = value;
    return copy;
}

// ------------------------------------------------------------------------------------ stream

/// Sixty-four bytes, the size of a quad batch's instance and of a cache line.
struct alignas(64) Record {
    std::array<float, 16> values{};
};

static_assert(sizeof(Record) == 64);

/// Read one record and write another: as fast as a record can move.
///
/// The record is copied whole. Copied field by field, as it was first written, it compiled to
/// sixteen scalar stores a record, which at two stores a cycle is what the row then measured —
/// the same 2.3 ns from 32 KiB to 64 MiB on the M4 Pro, the benchmark's own instructions rather
/// than the machine's memory. A machine row has to be limited by the machine.
[[nodiscard]] Result stream(std::size_t bytes_touched, std::string_view label) {
    // Reads and writes together, so a pass over this many records touches `bytes_touched`.
    const std::size_t records = bytes_touched / (2 * sizeof(Record));
    const std::uint64_t passes = std::max<std::uint64_t>(1, kUnitsPerIteration / records);

    std::vector<Record> in(records);
    for (std::size_t i = 0; i < records; ++i) {
        for (std::size_t field = 0; field < in[i].values.size(); ++field) {
            in[i].values[field] = static_cast<float>((i * 16) + field);
        }
    }
    std::vector<Record> out(records);
    std::uint64_t pass_counter = 0;

    auto result = measure("machine/stream", label, 15, 2, [&] {
        for (std::uint64_t pass = 0; pass < passes; ++pass) {
            // The last field says which pass wrote the record, so every pass writes something
            // the previous one did not, and the check below can tell the last pass happened.
            const auto stamp = static_cast<float>(pass_counter % 1024);
            for (std::size_t i = 0; i < records; ++i) {
                out[i] = in[i];
                out[i].values.back() = stamp;
            }
            ++pass_counter;
            clobber_memory();
        }
    });

    // Every record, every field: what was read arrived, and the last pass wrote it.
    const auto last_stamp = static_cast<float>((pass_counter - 1) % 1024);
    for (std::size_t i = 0; i < records; ++i) {
        for (std::size_t field = 0; field + 1 < in[i].values.size(); ++field) {
            if (out[i].values[field] != in[i].values[field]) {
                die(std::format("stream {}: record {} field {} was not copied", label, i, field));
            }
        }
        if (out[i].values.back() != last_stamp) {
            die(std::format("stream {}: record {} was not written by the last pass", label, i));
        }
    }

    result.units_per_iteration = passes * records;
    result.unit_name = "records";
    return result;
}

// ------------------------------------------------------------------------------------- chase

/// One node a cache line, so that no two loads share a line and a prefetcher that fetches the
/// next line along gains nothing. The link is a pointer, not an index: an index costs an
/// address calculation before every load, which is arithmetic, not latency.
struct alignas(64) Node {
    const Node* next = nullptr;
};

static_assert(sizeof(Node) == 64);

/// A deterministic generator for building the cycle. Not the engine's: the engine's streams
/// are for simulation, and nothing here is simulation.
[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

/// Follow a random cycle through every node, one dependent load at a time. Each load's address
/// is the previous load's value, so nothing overlaps: the time a step takes is the latency of
/// wherever that working set lives.
[[nodiscard]] Result chase(std::size_t bytes, std::string_view label) {
    const std::size_t count = bytes / sizeof(Node);
    if (count < 2 || count > kMaxNodes) {
        die(std::format("chase {}: {} nodes cannot form a cycle here", label, count));
    }

    // Sattolo's algorithm: a uniformly random permutation that is one cycle through all of them.
    std::vector<std::uint32_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    std::uint64_t seed = 0x4154'4C41'534D'3239ULL;  // "ATLASM29"
    for (std::size_t i = count - 1; i > 0; --i) {
        const auto j = static_cast<std::size_t>(splitmix64(seed) % i);
        std::swap(order[i], order[j]);
    }
    std::vector<Node> nodes(count);
    for (std::size_t i = 0; i < count; ++i) {
        nodes[i].next = &nodes[order[i]];
    }
    const auto index_of = [&](const Node* node) {
        return static_cast<std::uint32_t>(node - nodes.data());
    };

    // One cycle, visiting every node once: in what order, for the check below.
    std::vector<std::uint32_t> visit(count);
    std::vector<bool> seen(count, false);
    const Node* at = nodes.data();
    for (std::size_t step = 0; step < count; ++step) {
        const auto index = index_of(at);
        if (seen[index]) {
            die(std::format("chase {}: node {} visited twice; not one cycle", label, index));
        }
        seen[index] = true;
        visit[step] = index;
        at = at->next;
    }
    if (at != nodes.data()) {
        die(std::format("chase {}: the walk did not return to its start", label));
    }

    constexpr std::uint64_t kSteps = kUnitsPerIteration;
    const Node* current = nodes.data();
    std::uint64_t taken = 0;

    auto result = measure("machine/chase", label, 10, 1, [&] {
        const Node* p = current;
        for (std::uint64_t step = 0; step < kSteps; ++step) {
            p = p->next;
        }
        current = p;
        taken += kSteps;
    });

    // Where the walk ended, found another way: along the recorded visiting order.
    const std::uint32_t expected = visit[taken % count];
    if (index_of(current) != expected) {
        die(std::format("chase {}: ended at node {}, expected {}", label, index_of(current),
                        expected));
    }

    result.units_per_iteration = kSteps;
    result.unit_name = "loads";
    return result;
}

// ------------------------------------------------------------------------------------- chain

/// x -> m * x + b, modulo 2^64.
struct Affine {
    std::uint64_t m = 1;
    std::uint64_t b = 0;
};

/// `outer` after `inner`.
[[nodiscard]] Affine compose(Affine outer, Affine inner) {
    return Affine{.m = outer.m * inner.m, .b = (outer.m * inner.b) + outer.b};
}

/// The map applied `times` times, by squaring: the chain's end found without walking it.
[[nodiscard]] Affine power(Affine map, std::uint64_t times) {
    Affine result{};
    while (times > 0) {
        if ((times & 1U) != 0) {
            result = compose(map, result);
        }
        map = compose(map, map);
        times >>= 1U;
    }
    return result;
}

/// A dependent multiply and add, the latency of one core with nothing to overlap. The constants
/// are read through `opaque` so that the compiler cannot fold two steps into one, which it may
/// legally do for constants it knows: (a(ax + c) + c) is a²x + (ac + c).
[[nodiscard]] Result chain() {
    const Affine step{.m = opaque<std::uint64_t>(6364136223846793005ULL),
                      .b = opaque<std::uint64_t>(1442695040888963407ULL)};
    constexpr std::uint64_t kSteps = std::uint64_t{4} << 20;
    constexpr std::uint64_t kStart = 0x4154'4C41'534D'3239ULL;

    std::uint64_t x = kStart;
    std::uint64_t taken = 0;

    auto result = measure("machine/chain", "4M steps", 20, 2, [&] {
        std::uint64_t value = x;
        for (std::uint64_t i = 0; i < kSteps; ++i) {
            value = (value * step.m) + step.b;
        }
        x = value;
        taken += kSteps;
    });

    const Affine walked = power(step, taken);
    if (x != (walked.m * kStart) + walked.b) {
        die("chain: the end of the chain differs from its closed form");
    }

    result.units_per_iteration = kSteps;
    result.unit_name = "steps";
    return result;
}

[[nodiscard]] std::vector<Result> run() {
    std::vector<Result> results;
    results.push_back(stream(std::size_t{32} << 10, "32 KiB"));
    results.push_back(stream(std::size_t{1} << 20, "1 MiB"));
    results.push_back(stream(std::size_t{8} << 20, "8 MiB"));
    results.push_back(stream(std::size_t{64} << 20, "64 MiB"));
    results.push_back(chase(std::size_t{32} << 10, "32 KiB"));
    results.push_back(chase(std::size_t{1} << 20, "1 MiB"));
    results.push_back(chase(std::size_t{64} << 20, "64 MiB"));
    results.push_back(chain());
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("machine", run);

}  // namespace

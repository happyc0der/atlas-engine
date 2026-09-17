// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/tasks/worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <vector>

using atlas::tasks::Partition;
using atlas::tasks::partition_of;
using atlas::tasks::WorkerPool;

namespace {

/// Every worker count the milestone's exit criterion names, plus zero, which is the answer
/// everything else is compared against.
[[nodiscard]] std::vector<std::size_t> worker_counts() {
    std::vector<std::size_t> counts{0, 1, 2, 4};
    const std::size_t machine = WorkerPool::default_worker_count();
    if (machine > 4) {
        counts.push_back(machine);
    }
    return counts;
}

}  // namespace

TEST_CASE("a range is divided the same way whatever runs it", "[tasks][determinism]") {
    // The whole determinism argument rests on this: the chunks depend on the count and the
    // grain and on nothing else, so no worker count can change what any chunk computes.
    STATIC_REQUIRE(true);
    const Partition ten = partition_of(100, 10);
    CHECK(ten.chunks == 10);
    CHECK(ten.chunk_size == 10);

    // A count that does not divide evenly leaves a short last chunk rather than an empty one.
    const Partition ragged = partition_of(95, 10);
    CHECK(ragged.chunks == 10);
    CHECK(ragged.chunk_size == 10);

    CHECK(partition_of(0, 10).chunks == 0);
    // A grain of zero is one item per chunk, not a division by zero.
    CHECK(partition_of(7, 0).chunks == 7);
    CHECK(partition_of(7, 0).chunk_size == 1);
    // A grain larger than the range is one chunk.
    CHECK(partition_of(5, 1000).chunks == 1);
}

TEST_CASE("a pool refuses more workers than it will create", "[tasks]") {
    const auto too_many = WorkerPool::create(WorkerPool::kMaxWorkers + 1);
    REQUIRE_FALSE(too_many.has_value());
    CHECK(too_many.error().code() == atlas::ErrorCode::InvalidArgument);
    CHECK(WorkerPool::create(WorkerPool::kMaxWorkers).has_value());
}

TEST_CASE("no workers means the calling thread does the work", "[tasks]") {
    auto pool = WorkerPool::create(0).value();
    CHECK(pool.worker_count() == 0);

    std::vector<int> touched(1000, 0);
    pool.parallel_for(touched.size(), 64, [&touched](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            touched[i] = 1;
        }
    });
    CHECK(std::accumulate(touched.begin(), touched.end(), 0) == 1000);
}

TEST_CASE("every index is visited exactly once, at every worker count", "[tasks][determinism]") {
    // The property a parallel loop has to have before anything else about it matters.
    for (const std::size_t workers : worker_counts()) {
        INFO("workers " << workers);
        auto pool = WorkerPool::create(workers).value();
        for (const std::size_t count :
             {std::size_t{0}, std::size_t{1}, std::size_t{999}, std::size_t{4096}}) {
            std::vector<std::atomic<int>> visits(count);
            pool.parallel_for(count, 37, [&visits](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    visits[i].fetch_add(1, std::memory_order_relaxed);
                }
            });
            for (std::size_t i = 0; i < count; ++i) {
                REQUIRE(visits[i].load(std::memory_order_relaxed) == 1);
            }
        }
    }
}

TEST_CASE("the result does not depend on how many workers ran it", "[tasks][determinism]") {
    // M8's exit criterion in miniature: the same computation at one, two, four and the
    // machine's worth of workers, compared against the answer with none at all. The body
    // writes only its own indices, which is the contract that makes this hold.
    constexpr std::size_t kCount = 10'000;
    const auto compute = [](std::size_t i) {
        return static_cast<std::uint64_t>(i) * 2'654'435'761ULL % 1'000'003ULL;
    };

    std::vector<std::uint64_t> expected(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        expected[i] = compute(i);
    }

    for (const std::size_t workers : worker_counts()) {
        for (const std::size_t grain : {std::size_t{1}, std::size_t{64}, std::size_t{9973}}) {
            INFO("workers " << workers << " grain " << grain);
            auto pool = WorkerPool::create(workers).value();
            std::vector<std::uint64_t> actual(kCount, 0);
            pool.parallel_for(kCount, grain,
                              [&actual, &compute](std::size_t begin, std::size_t end) {
                                  for (std::size_t i = begin; i < end; ++i) {
                                      actual[i] = compute(i);
                                  }
                              });
            REQUIRE(actual == expected);
        }
    }
}

TEST_CASE("a pool runs many loops in a row", "[tasks]") {
    // Workers sleep between loops, so waking them again is where a missed notification would
    // show up as a hang rather than as a wrong answer.
    auto pool = WorkerPool::create(4).value();
    std::atomic<std::uint64_t> total{0};
    for (int round = 0; round < 50; ++round) {
        pool.parallel_for(1000, 16, [&total](std::size_t begin, std::size_t end) {
            total.fetch_add(end - begin, std::memory_order_relaxed);
        });
    }
    CHECK(total.load() == std::uint64_t{50} * 1000);
}

TEST_CASE("a moved-from pool is destroyed without joining twice", "[tasks]") {
    auto first = WorkerPool::create(2).value();
    WorkerPool second = std::move(first);
    CHECK(second.worker_count() == 2);

    std::atomic<int> ran{0};
    second.parallel_for(
        100, 10, [&ran](std::size_t, std::size_t) { ran.fetch_add(1, std::memory_order_relaxed); });
    CHECK(ran.load() == 10);
}

TEST_CASE("an empty range does nothing at all", "[tasks]") {
    auto pool = WorkerPool::create(2).value();
    std::atomic<int> calls{0};
    pool.parallel_for(0, 8, [&calls](std::size_t, std::size_t) {
        calls.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(calls.load() == 0);
}

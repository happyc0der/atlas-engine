// SPDX-License-Identifier: GPL-3.0-or-later
// The inbox, with several threads pushing into it.
//
// The inbox is the only genuinely concurrent thing in this module, so this is the only file
// here that the thread sanitizer has anything to say about. It carries the "unit" label for
// that reason: the sanitizer presets select "unit" and "determinism" and nothing else, and a
// thread test the sanitizer does not run is a thread test that does not exist.
#include <atlas/core/assert.hpp>
#include <atlas/net/inbox.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using atlas::net::CommandInbox;

namespace {

const bool kMainThreadMarkedForThreads = [] {
    atlas::mark_main_thread();
    return true;
}();

constexpr std::size_t kProducers = 4;
constexpr std::size_t kPerProducer = 400;

/// A message naming its producer and its position in that producer's sequence.
[[nodiscard]] std::vector<std::byte> tagged(std::size_t producer, std::size_t index) {
    return {
        static_cast<std::byte>(producer),
        static_cast<std::byte>(index & 0xFFU),
        static_cast<std::byte>((index >> 8) & 0xFFU),
    };
}

}  // namespace

TEST_CASE("many producers lose nothing, duplicate nothing, and keep their own order",
          "[net][inbox]") {
    // Drained repeatedly while the producers are still running, which is what a frame loop
    // does. The bound is deliberately not reached: this case is about the lock rather than
    // about overflow, and an inbox that overflowed here would be testing the wrong thing.
    CommandInbox inbox;
    std::vector<std::vector<std::byte>> collected;

    // The consumer stops when the producers have finished and the mailbox is empty, rather than
    // when it has collected what it expected. Waiting for a count went wrong twice over: against
    // an inbox that silently evicted instead of refusing, the count is never reached and the
    // loop spins for ever; and an arbitrary iteration bound instead turned a slow machine into a
    // failure — on a sanitizer runner the consumer span a million times while the producers had
    // managed 714 of 1600, which is not a defect. Ending on the condition that actually matters
    // catches the eviction in the assertion afterwards and never mistakes slowness for loss.
    std::atomic<std::size_t> finished{0};

    std::vector<std::jthread> producers;
    producers.reserve(kProducers);
    for (std::size_t producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&inbox, &finished, producer] {
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                // Retried rather than dropped: this case is about the handover, not the bound.
                while (inbox.push(tagged(producer, i)) != CommandInbox::Push::Accepted) {
                    std::this_thread::yield();
                }
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    std::vector<std::vector<std::byte>> batch;
    while (finished.load(std::memory_order_acquire) < kProducers || inbox.depth() > 0) {
        inbox.drain(batch);
        collected.insert(collected.end(), std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
        std::this_thread::yield();
    }
    for (auto& producer : producers) {
        producer.join();
    }
    inbox.drain(batch);
    collected.insert(collected.end(), std::make_move_iterator(batch.begin()),
                     std::make_move_iterator(batch.end()));

    INFO("collected " << collected.size());
    REQUIRE(collected.size() == kProducers * kPerProducer);
    CHECK(inbox.accepted() == kProducers * kPerProducer);

    // Every message exactly once. A lock dropped on the fast path shows up here as a message
    // that was written over rather than appended.
    std::vector<std::vector<std::size_t>> seen(kProducers);
    for (const auto& message : collected) {
        REQUIRE(message.size() == 3);
        const auto producer = std::to_integer<std::size_t>(message[0]);
        REQUIRE(producer < kProducers);
        const std::size_t index = std::to_integer<std::size_t>(message[1]) |
                                  (std::to_integer<std::size_t>(message[2]) << 8);
        seen[producer].push_back(index);
    }

    for (std::size_t producer = 0; producer < kProducers; ++producer) {
        INFO("producer " << producer);
        REQUIRE(seen[producer].size() == kPerProducer);
        // Each producer's own messages arrive in the order it pushed them. Across producers
        // there is no order to keep and none is claimed: the total order that matters is
        // (source, sequence) inside the command queue, not arrival here.
        CHECK(std::ranges::is_sorted(seen[producer]));
        CHECK(std::ranges::adjacent_find(seen[producer]) == seen[producer].end());
    }
}

TEST_CASE("a producer that overruns the bound is refused rather than racing", "[net][inbox]") {
    // Nobody drains. Every producer therefore hits the bound, and what is being checked is that
    // the refusal is as thread-safe as the acceptance: the counts must add up exactly.
    CommandInbox inbox;
    std::vector<std::jthread> producers;
    producers.reserve(kProducers);
    for (std::size_t producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&inbox, producer] {
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                (void)inbox.push(tagged(producer, i));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    CHECK(inbox.overflowed());
    CHECK(inbox.accepted() + inbox.refused() == kProducers * kPerProducer);
    CHECK(inbox.accepted() <= CommandInbox::kMaxMessages);
    CHECK(inbox.depth() == inbox.accepted());
}

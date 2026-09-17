// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/snapshot.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using atlas::sim::SnapshotChannel;
using atlas::sim::SnapshotHeader;

namespace {

/// Presentation data, not state: what to draw, not the tables it came from.
struct Frame {
    SnapshotHeader header;
    std::vector<std::uint16_t> colour_index;
};

using Channel = SnapshotChannel<Frame>;

[[nodiscard]] std::shared_ptr<const Frame> make_frame(std::uint64_t tick, std::size_t cells) {
    auto frame = std::make_shared<Frame>();
    frame->header.tick = tick;
    frame->header.state_hash = tick * 31;
    frame->header.generation = tick;
    frame->colour_index.assign(cells, static_cast<std::uint16_t>(tick));
    return frame;
}

}  // namespace

TEST_CASE("a channel starts empty", "[sim][snapshot]") {
    const Channel channel;
    CHECK(channel.empty());
    CHECK(channel.latest() == nullptr);
    CHECK(channel.published() == 0);
}

TEST_CASE("a published snapshot can be taken", "[sim][snapshot]") {
    Channel channel;
    channel.publish(make_frame(7, 4));

    const auto taken = channel.latest();
    REQUIRE(taken != nullptr);
    CHECK(taken->header.tick == 7);
    CHECK(taken->colour_index.size() == 4);
    CHECK(channel.published() == 1);
    CHECK_FALSE(channel.empty());
}

TEST_CASE("taking does not consume", "[sim][snapshot]") {
    // The renderer may ask twice within a frame, and a frame that drew half from one
    // snapshot and half from nothing would be worse than one that repeated itself.
    Channel channel;
    channel.publish(make_frame(1, 2));

    CHECK(channel.latest() != nullptr);
    CHECK(channel.latest() != nullptr);
    CHECK(channel.latest()->header.tick == 1);
}

TEST_CASE("the latest wins and older ones are dropped", "[sim][snapshot]") {
    // No queue. A renderer that fell behind would be drawing history, and the backlog would
    // only grow.
    Channel channel;
    for (std::uint64_t tick = 1; tick <= 5; ++tick) {
        channel.publish(make_frame(tick, 1));
    }

    CHECK(channel.latest()->header.tick == 5);
    CHECK(channel.published() == 5);
}

TEST_CASE("a held snapshot survives being replaced", "[sim][snapshot]") {
    // The reason for the shared pointer: the renderer keeps drawing from a snapshot the
    // simulation has already moved past, and the contents must stay valid for the whole
    // frame.
    Channel channel;
    channel.publish(make_frame(1, 3));

    const auto held = channel.latest();
    REQUIRE(held != nullptr);

    for (std::uint64_t tick = 2; tick <= 100; ++tick) {
        channel.publish(make_frame(tick, 3));
    }

    // Unchanged, and still readable.
    CHECK(held->header.tick == 1);
    CHECK(held->colour_index.size() == 3);
    CHECK(held->colour_index[0] == 1);
    CHECK(channel.latest()->header.tick == 100);
}

TEST_CASE("clearing empties the channel", "[sim][snapshot]") {
    Channel channel;
    channel.publish(make_frame(1, 1));
    channel.clear();
    CHECK(channel.empty());
    // The count is of publications, not of what is held, so clearing does not rewrite it.
    CHECK(channel.published() == 1);
}

TEST_CASE("a consumer never sees a torn snapshot", "[sim][snapshot]") {
    // The property that has to hold when the simulation moves off the main thread in M8.
    // The consumer checks that every snapshot it takes is internally consistent: the tick in
    // the header matches every cell, so a half-published one would be visible.
    Channel channel;
    channel.publish(make_frame(0, 256));

    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> seen{0};

    std::thread producer([&channel, &stop] {
        for (std::uint64_t tick = 1; !stop.load(std::memory_order_relaxed); ++tick) {
            channel.publish(make_frame(tick, 256));
        }
    });

    std::thread consumer([&channel, &stop, &torn, &seen] {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto frame = channel.latest();
            if (frame == nullptr) {
                continue;
            }
            seen.fetch_add(1, std::memory_order_relaxed);

            const auto expected = static_cast<std::uint16_t>(frame->header.tick);
            for (const std::uint16_t cell : frame->colour_index) {
                if (cell != expected) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    });

    // Bounded by iterations rather than by a clock, so the test takes the same work on every
    // machine. It also waits for the consumer to have seen at least one frame: on a
    // two-processor runner the producer published two thousand frames before the consumer
    // thread was ever scheduled, and the torn check then passed over nothing, which is what
    // the seen check below exists to catch. Waiting on it makes both assertions meaningful
    // rather than one of them a statement about the scheduler.
    while (channel.published() < 2000 || seen.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    consumer.join();

    CHECK(torn.load() == 0);
    CHECK(seen.load() > 0);
}

TEST_CASE("a snapshot outlives the channel", "[sim][snapshot]") {
    // A frame in flight when the simulation shuts down must still be drawable, or shutdown
    // becomes ordered with respect to rendering.
    std::shared_ptr<const Frame> held;
    {
        Channel channel;
        channel.publish(make_frame(42, 8));
        held = channel.latest();
    }
    REQUIRE(held != nullptr);
    CHECK(held->header.tick == 42);
    CHECK(held->colour_index.size() == 8);
}

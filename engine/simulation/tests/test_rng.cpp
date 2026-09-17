// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/rng.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

using atlas::sim::RngStream;
using atlas::sim::RngStreams;
using atlas::sim::stream_id;

namespace {

[[nodiscard]] std::vector<std::uint64_t> draw(RngStream stream, std::size_t count) {
    std::vector<std::uint64_t> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(stream.next_u64());
    }
    return out;
}

}  // namespace

TEST_CASE("the same key produces the same sequence", "[sim][rng]") {
    const RngStreams streams{12345, 7};
    CHECK(draw(streams.stream("movement"), 16) == draw(streams.stream("movement"), 16));
}

TEST_CASE("a different seed, stream or tick produces a different sequence", "[sim][rng]") {
    const RngStreams base{12345, 7};
    const auto reference = draw(base.stream("movement"), 8);

    CHECK(draw(RngStreams{12346, 7}.stream("movement"), 8) != reference);
    CHECK(draw(base.stream("weather"), 8) != reference);
    CHECK(draw(RngStreams{12345, 8}.stream("movement"), 8) != reference);
}

TEST_CASE("a stream does not depend on what other streams consumed", "[sim][rng]") {
    // The reason for counter-based generation. With one shared stateful generator, adding a
    // single call anywhere shifts every later value everywhere, and a replay stops matching
    // for a reason unrelated to the change.
    const RngStreams streams{999, 42};

    auto alone = streams.stream("movement");
    const std::uint64_t first_alone = alone.next_u64();
    const std::uint64_t second_alone = alone.next_u64();

    auto movement = streams.stream("movement");
    auto weather = streams.stream("weather");

    const std::uint64_t first = movement.next_u64();
    for (int i = 0; i < 1000; ++i) {
        (void)weather.next_u64();
    }
    const std::uint64_t second = movement.next_u64();

    CHECK(first == first_alone);
    CHECK(second == second_alone);
}

TEST_CASE("the counter is part of the key and advances", "[sim][rng]") {
    const RngStreams streams{1, 1};
    auto stream = streams.stream("a");
    CHECK(stream.counter() == 0);
    (void)stream.next_u64();
    (void)stream.next_u64();
    CHECK(stream.counter() == 2);
}

TEST_CASE("a stream can be resumed from a recorded position", "[sim][rng]") {
    // What restoring from a save needs: the position is data, so it can be written down and
    // put back.
    const RngStreams streams{5, 5};
    auto original = streams.stream("a");
    for (int i = 0; i < 10; ++i) {
        (void)original.next_u64();
    }
    const std::uint64_t expected = original.next_u64();

    auto resumed = streams.stream("a");
    resumed.set_counter(10);
    CHECK(resumed.next_u64() == expected);
}

TEST_CASE("keys that would collide under a naive mix do not", "[sim][rng]") {
    // Adding the key parts together would make stream 2 at counter 3 meet stream 3 at
    // counter 2. Each part is folded in with its own multiplier so they cannot.
    const RngStreams streams{0, 0};

    auto a = RngStream{0, atlas::sim::StreamId{2}, 0};
    a.set_counter(3);
    auto b = RngStream{0, atlas::sim::StreamId{3}, 0};
    b.set_counter(2);
    CHECK(a.next_u64() != b.next_u64());

    // Likewise tick against counter.
    auto c = RngStream{0, atlas::sim::StreamId{0}, 4};
    c.set_counter(1);
    auto d = RngStream{0, atlas::sim::StreamId{0}, 1};
    d.set_counter(4);
    CHECK(c.next_u64() != d.next_u64());

    (void)streams;
}

TEST_CASE("a zero key still produces varied output", "[sim][rng]") {
    // A weak mixer returns zero for an all-zero key, and seed zero at tick zero is exactly
    // what a default-constructed simulation would use.
    const auto stream = RngStreams{0, 0}.stream("");
    const auto values = draw(stream, 8);
    for (const std::uint64_t value : values) {
        CHECK(value != 0);
    }
    CHECK(values[0] != values[1]);
}

TEST_CASE("next_below stays inside its bound", "[sim][rng]") {
    auto stream = RngStreams{77, 3}.stream("bounded");
    for (int i = 0; i < 10'000; ++i) {
        CHECK(stream.next_below(6) < 6);
    }
}

TEST_CASE("a bound of zero or one is handled", "[sim][rng]") {
    auto stream = RngStreams{1, 1}.stream("edge");
    CHECK(stream.next_below(0) == 0);
    for (int i = 0; i < 100; ++i) {
        CHECK(stream.next_below(1) == 0);
    }
}

TEST_CASE("next_below is not visibly biased", "[sim][rng]") {
    // A remainder-based bound favours the low values. This will not catch a subtle bias, but
    // it does catch the obvious one, and the method is chosen to have none.
    constexpr std::uint64_t kBound = 7;
    constexpr int kDraws = 70'000;

    std::array<int, kBound> counts{};
    auto stream = RngStreams{2024, 11}.stream("uniform");
    for (int i = 0; i < kDraws; ++i) {
        counts[stream.next_below(kBound)] += 1;
    }

    constexpr int expected = kDraws / static_cast<int>(kBound);
    for (std::size_t i = 0; i < kBound; ++i) {
        INFO("outcome " << i << " occurred " << counts[i] << " times, expected near " << expected);
        CHECK(counts[i] > (expected * 9 / 10));
        CHECK(counts[i] < (expected * 11 / 10));
    }
}

TEST_CASE("a huge bound is handled", "[sim][rng]") {
    auto stream = RngStreams{3, 3}.stream("huge");
    constexpr std::uint64_t kBound = (std::uint64_t{1} << 63) + 1;
    for (int i = 0; i < 1000; ++i) {
        CHECK(stream.next_below(kBound) < kBound);
    }
}

TEST_CASE("a range includes both ends and never leaves them", "[sim][rng]") {
    auto stream = RngStreams{8, 8}.stream("range");
    bool saw_low = false;
    bool saw_high = false;
    for (int i = 0; i < 20'000; ++i) {
        const std::int64_t value = stream.next_in_range(-3, 3);
        REQUIRE(value >= -3);
        REQUIRE(value <= 3);
        saw_low = saw_low || value == -3;
        saw_high = saw_high || value == 3;
    }
    CHECK(saw_low);
    CHECK(saw_high);
}

TEST_CASE("an inverted or empty range yields its low value", "[sim][rng]") {
    auto stream = RngStreams{9, 9}.stream("range");
    CHECK(stream.next_in_range(5, 5) == 5);
    CHECK(stream.next_in_range(5, 1) == 5);
}

TEST_CASE("a range spanning the whole type does not overflow", "[sim][rng]") {
    // high - low overflows a signed 64-bit integer here, so the span is computed unsigned.
    auto stream = RngStreams{10, 10}.stream("wide");
    constexpr std::int64_t low = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t high = std::numeric_limits<std::int64_t>::max();
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t value = stream.next_in_range(low, high);
        CHECK(value >= low);
        CHECK(value <= high);
    }
}

TEST_CASE("stream names are distinct identifiers", "[sim][rng]") {
    CHECK(stream_id("movement") == stream_id("movement"));
    CHECK(stream_id("movement") != stream_id("weather"));
    CHECK(stream_id("") == stream_id(""));
}

TEST_CASE("the mixer spreads its output", "[sim][rng]") {
    // Not a statistical test; a check that consecutive draws are not obviously structured,
    // which a broken mixer would make them.
    std::map<std::uint64_t, int> seen;
    auto stream = RngStreams{4242, 1}.stream("spread");
    for (int i = 0; i < 4096; ++i) {
        seen[stream.next_u64()] += 1;
    }
    CHECK(seen.size() == 4096);

    // The top byte should visit most of its range rather than a handful of values.
    std::map<std::uint64_t, int> top;
    auto again = RngStreams{4242, 1}.stream("spread");
    for (int i = 0; i < 4096; ++i) {
        top[again.next_u64() >> 56] += 1;
    }
    CHECK(top.size() > 200);
}

TEST_CASE("32-bit draws come from the strong half", "[sim][rng]") {
    auto reference = RngStreams{31, 2}.stream("half");
    auto narrow = RngStreams{31, 2}.stream("half");
    CHECK(narrow.next_u32() == static_cast<std::uint32_t>(reference.next_u64() >> 32));
}

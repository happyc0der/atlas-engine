// SPDX-License-Identifier: GPL-3.0-or-later
// Clip evaluation, with no scene, no device and no clock.
//
// This is where the arithmetic that can be wrong lives. Every assertion here is exact or within
// a stated tolerance, and the exactness is designed in rather than hoped for: the easings are
// chosen to be exact at their endpoints and the clock is integer throughout, so a key's own
// value compares with `==`.
#include <atlas/animation/clip.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using atlas::animation::cell_uv;
using atlas::animation::Clip;
using atlas::animation::clip_time;
using atlas::animation::ease;
using atlas::animation::Easing;
using atlas::animation::FrameGrid;
using atlas::animation::FrameKey;
using atlas::animation::LoopMode;
using atlas::animation::sample;
using atlas::animation::TransformKey;
using Catch::Approx;

namespace {

constexpr std::uint64_t kMillisecond = 1'000'000ULL;
constexpr std::uint64_t kSecond = 1'000'000'000ULL;

/// A clip that moves from the origin to (10, 0) over one second.
[[nodiscard]] Clip straight_line(Easing easing = Easing::Linear) {
    Clip clip;
    clip.name = "line";
    clip.duration_ns = kSecond;
    clip.transform_keys = {
        TransformKey{.time_ns = 0, .position_offset = {.x = 0.0F, .y = 0.0F}, .easing = easing},
        TransformKey{.time_ns = kSecond, .position_offset = {.x = 10.0F, .y = 0.0F}},
    };
    return clip;
}

}  // namespace

TEST_CASE("every easing is exact at both endpoints", "[animation][clip]") {
    // The property the whole design rests on. If an easing is a rounding error away from one at
    // its end, a key's own authored value is never quite reached, and every test below would
    // need a tolerance instead of an equality.
    for (std::uint8_t raw = 0; raw < static_cast<std::uint8_t>(Easing::Count); ++raw) {
        const auto easing = static_cast<Easing>(raw);
        INFO("easing " << atlas::animation::to_string(easing));
        CHECK(ease(easing, 0.0F) == 0.0F);
        CHECK(ease(easing, 1.0F) == 1.0F);
    }
}

TEST_CASE("easings are bounded and monotonic in the middle", "[animation][clip]") {
    for (std::uint8_t raw = 0; raw < static_cast<std::uint8_t>(Easing::Count); ++raw) {
        const auto easing = static_cast<Easing>(raw);
        INFO("easing " << atlas::animation::to_string(easing));
        float previous = 0.0F;
        for (int step = 0; step <= 20; ++step) {
            const float t = static_cast<float>(step) / 20.0F;
            const float value = ease(easing, t);
            CHECK(value >= 0.0F);
            CHECK(value <= 1.0F);
            // Never goes backwards. An easing that overshoots and returns is a different
            // feature; none of these is meant to be one.
            CHECK(value >= previous);
            previous = value;
        }
    }
}

TEST_CASE("linear easing is the identity and step holds", "[animation][clip]") {
    CHECK(ease(Easing::Linear, 0.25F) == 0.25F);
    CHECK(ease(Easing::Linear, 0.75F) == 0.75F);

    // Step holds this key's value for the whole segment. Anything that should snap rather than
    // slide uses it, and a frame track does not need it because frames never interpolate.
    CHECK(ease(Easing::Step, 0.0F) == 0.0F);
    CHECK(ease(Easing::Step, 0.99F) == 0.0F);
    CHECK(ease(Easing::Step, 1.0F) == 1.0F);
}

TEST_CASE("easing input outside the segment is clamped", "[animation][clip]") {
    // Extrapolating a curve past its ends makes it a different curve. Every caller here has
    // already bounded the input; this is what happens if one ever stops.
    CHECK(ease(Easing::CubicIn, -1.0F) == 0.0F);
    CHECK(ease(Easing::CubicIn, 2.0F) == 1.0F);
}

TEST_CASE("a clip samples its own keys exactly", "[animation][clip]") {
    const Clip clip = straight_line();
    CHECK(sample(clip, 0).position_offset.x == 0.0F);
    CHECK(sample(clip, kSecond).position_offset.x == 10.0F);
}

TEST_CASE("the midpoint of a linear segment is the midpoint", "[animation][clip]") {
    const Clip clip = straight_line();
    CHECK(sample(clip, kSecond / 2).position_offset.x == Approx(5.0F).margin(1e-4));
}

TEST_CASE("a clip holds before its first key and after its last", "[animation][clip]") {
    Clip clip;
    clip.duration_ns = 4 * kSecond;
    clip.transform_keys = {
        TransformKey{.time_ns = kSecond, .position_offset = {.x = 3.0F, .y = 0.0F}},
        TransformKey{.time_ns = 2 * kSecond, .position_offset = {.x = 7.0F, .y = 0.0F}},
    };

    // Before the first key, the first key's value: a clip whose track starts late is not
    // undefined for the time before it, it simply has not begun moving.
    CHECK(sample(clip, 0).position_offset.x == 3.0F);
    CHECK(sample(clip, kSecond / 2).position_offset.x == 3.0F);
    // And after the last, held rather than wrapped. Wrapping is the loop mode's job.
    CHECK(sample(clip, 3 * kSecond).position_offset.x == 7.0F);
}

TEST_CASE("an empty transform track contributes the identity", "[animation][clip]") {
    // A clip with only frames must leave the transform alone rather than snapping it to zero,
    // or attaching a frame animation to a placed entity would move it to the origin.
    Clip clip;
    clip.duration_ns = kSecond;
    clip.frame_keys = {FrameKey{.time_ns = 0, .cell = 2}};

    const auto sampled = sample(clip, kSecond / 2);
    CHECK(sampled.position_offset.x == 0.0F);
    CHECK(sampled.position_offset.y == 0.0F);
    CHECK(sampled.rotation_offset == 0.0F);
    CHECK(sampled.scale_factor.x == 1.0F);
    CHECK(sampled.scale_factor.y == 1.0F);
    CHECK(sampled.has_cell);
    CHECK(sampled.cell == 2);
}

TEST_CASE("a frame track never interpolates", "[animation][clip]") {
    // A sheet shows one cell or another; there is nothing between two drawings. The key in
    // force is the last one reached, and it stays in force until the next.
    Clip clip;
    clip.duration_ns = 4 * kSecond;
    clip.frame_keys = {
        FrameKey{.time_ns = 0, .cell = 0},
        FrameKey{.time_ns = kSecond, .cell = 1},
        FrameKey{.time_ns = 2 * kSecond, .cell = 2},
    };

    CHECK(sample(clip, 0).cell == 0);
    CHECK(sample(clip, kSecond - 1).cell == 0);
    CHECK(sample(clip, kSecond).cell == 1);
    CHECK(sample(clip, kSecond + (kSecond / 2)).cell == 1);
    CHECK(sample(clip, 2 * kSecond).cell == 2);
    CHECK(sample(clip, 10 * kSecond).cell == 2);
}

TEST_CASE("rotation is not wrapped to a turn", "[animation][clip]") {
    // A key at two pi means one revolution, not zero. Wrapping would make a spin inexpressible,
    // and a shortest-arc blend would make it go the wrong way round for anything past a half.
    Clip clip;
    clip.duration_ns = kSecond;
    clip.transform_keys = {
        TransformKey{.time_ns = 0, .rotation_offset = 0.0F},
        TransformKey{.time_ns = kSecond, .rotation_offset = 2.0F * std::numbers::pi_v<float>},
    };

    CHECK(sample(clip, kSecond).rotation_offset ==
          Approx(2.0F * std::numbers::pi_v<float>).margin(1e-5));
    // Halfway is half a turn, not the shortest way back to zero.
    CHECK(sample(clip, kSecond / 2).rotation_offset ==
          Approx(std::numbers::pi_v<float>).margin(1e-4));
}

TEST_CASE("a clip that plays once holds at its end", "[animation][clip]") {
    CHECK(clip_time(0, kSecond, LoopMode::Once) == 0);
    CHECK(clip_time(kSecond / 2, kSecond, LoopMode::Once) == kSecond / 2);
    CHECK(clip_time(kSecond, kSecond, LoopMode::Once) == kSecond);
    // Held at the duration itself, not one past it, so the last key's value is what shows.
    CHECK(clip_time(1000 * kSecond, kSecond, LoopMode::Once) == kSecond);
}

TEST_CASE("a looping clip wraps exactly", "[animation][clip]") {
    CHECK(clip_time(kSecond, kSecond, LoopMode::Loop) == 0);
    CHECK(clip_time(kSecond + 5, kSecond, LoopMode::Loop) == 5);
    CHECK(clip_time((1000 * kSecond) + 7, kSecond, LoopMode::Loop) == 7);
}

TEST_CASE("a ping-pong clip turns at both ends", "[animation][clip]") {
    constexpr std::uint64_t kDuration = 100;
    CHECK(clip_time(0, kDuration, LoopMode::PingPong) == 0);
    CHECK(clip_time(50, kDuration, LoopMode::PingPong) == 50);
    // The turning point lands exactly on the end rather than one step past it.
    CHECK(clip_time(100, kDuration, LoopMode::PingPong) == 100);
    CHECK(clip_time(150, kDuration, LoopMode::PingPong) == 50);
    CHECK(clip_time(200, kDuration, LoopMode::PingPong) == 0);
    CHECK(clip_time(250, kDuration, LoopMode::PingPong) == 50);
}

TEST_CASE("a clip of no length is not divided by", "[animation][clip]") {
    CHECK(clip_time(1000, 0, LoopMode::Loop) == 0);
    CHECK(clip_time(1000, 0, LoopMode::PingPong) == 0);
    CHECK(clip_time(1000, 0, LoopMode::Once) == 0);
}

TEST_CASE("the clock is exact after a million frames", "[animation][clip]") {
    // The reason the clock is integer. A float accumulating sixteen and two thirds
    // milliseconds a million times is nowhere near the right answer; this is exactly right,
    // and it is an equality rather than a tolerance.
    constexpr std::uint64_t kFrameNs = 16'666'667;
    constexpr std::uint64_t kFrames = 1'000'000;
    constexpr std::uint64_t kDuration = 4 * kSecond;

    std::uint64_t elapsed = 0;
    for (std::uint64_t frame = 0; frame < kFrames; ++frame) {
        elapsed += kFrameNs;
    }
    CHECK(elapsed == kFrameNs * kFrames);
    CHECK(clip_time(elapsed, kDuration, LoopMode::Loop) == ((kFrameNs * kFrames) % kDuration));
}

TEST_CASE("a grid cell is the rectangle it should be", "[animation][clip]") {
    const FrameGrid grid{.columns = 4, .rows = 2};

    const auto first = cell_uv(grid, 0);
    CHECK(first.position.x == 0.0F);
    CHECK(first.position.y == 0.0F);
    CHECK(first.size.x == Approx(0.25F));
    CHECK(first.size.y == Approx(0.5F));

    // Counted left to right then top to bottom, so cell 4 is the start of the second row.
    const auto second_row = cell_uv(grid, 4);
    CHECK(second_row.position.x == 0.0F);
    CHECK(second_row.position.y == Approx(0.5F));

    const auto last = cell_uv(grid, 7);
    CHECK(last.position.x == Approx(0.75F));
    CHECK(last.position.y == Approx(0.5F));
}

TEST_CASE("a cell past the end of the grid stays inside the texture", "[animation][clip]") {
    // Import refuses a clip that disagrees with its sheet. If one reaches here anyway, showing
    // the wrong frame is a better failure than sampling outside the texture.
    const FrameGrid grid{.columns = 2, .rows = 2};
    const auto wrapped = cell_uv(grid, 5);
    CHECK(wrapped.position.x == Approx(cell_uv(grid, 1).position.x));
    CHECK(wrapped.position.y == Approx(cell_uv(grid, 1).position.y));

    // And a grid of nothing is the whole texture rather than a division by zero.
    const auto whole = cell_uv(FrameGrid{.columns = 0, .rows = 0}, 3);
    CHECK(whole.size.x == 1.0F);
    CHECK(whole.size.y == 1.0F);
}

TEST_CASE("every easing and loop mode has a name", "[animation][clip]") {
    CHECK(atlas::animation::to_string(LoopMode::Once) == "once");
    CHECK(atlas::animation::to_string(LoopMode::Loop) == "loop");
    CHECK(atlas::animation::to_string(LoopMode::PingPong) == "ping-pong");
    for (std::uint8_t raw = 0; raw < static_cast<std::uint8_t>(Easing::Count); ++raw) {
        CHECK(atlas::animation::to_string(static_cast<Easing>(raw)) != "unrecognised");
    }
}

TEST_CASE("two keys at the same time are a snap", "[animation][clip]") {
    // A pair of keys sharing a time is how a clip says "jump here". Two things have to hold:
    // the span between them is zero and must not be divided by, and the jump takes effect at
    // that time rather than one step after it — a key at time T is in force from T.
    Clip clip;
    clip.duration_ns = kSecond;
    clip.transform_keys = {
        TransformKey{.time_ns = 0, .position_offset = {.x = 1.0F, .y = 0.0F}},
        TransformKey{.time_ns = 500 * kMillisecond, .position_offset = {.x = 2.0F, .y = 0.0F}},
        TransformKey{.time_ns = 500 * kMillisecond, .position_offset = {.x = 9.0F, .y = 0.0F}},
        TransformKey{.time_ns = kSecond, .position_offset = {.x = 9.0F, .y = 0.0F}},
    };

    // Just before: still sliding towards the first of the pair.
    CHECK(sample(clip, (500 * kMillisecond) - 1).position_offset.x == Approx(2.0F).margin(1e-4));
    // At the moment itself: jumped.
    CHECK(sample(clip, 500 * kMillisecond).position_offset.x == Approx(9.0F).margin(1e-4));
}

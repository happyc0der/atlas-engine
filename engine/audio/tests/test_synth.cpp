// SPDX-License-Identifier: GPL-3.0-or-later
// The generated sounds, which exist so a caller with no asset pipeline can still make one.
#include <atlas/audio/synth.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using atlas::audio::kMixSampleRate;
using atlas::audio::sine_blip;
using Catch::Approx;

TEST_CASE("a blip is as long as it was asked to be", "[audio][synth]") {
    const auto samples = sine_blip(440.0F, 100);
    CHECK(samples.size() == kMixSampleRate / 10);
}

TEST_CASE("a blip starts and ends at silence", "[audio][synth]") {
    const auto samples = sine_blip(440.0F, 100);
    REQUIRE(samples.size() > 2);

    // The fades are the point. A sine cut off mid-cycle has a step at each end, and a step is
    // heard as a click over the tone: the one artefact that makes a generated sound read as a
    // bug rather than as a sound.
    CHECK(samples.front() == Approx(0.0F).margin(1e-6));
    CHECK(samples.back() == Approx(0.0F).margin(1e-6));
}

TEST_CASE("a blip stays inside its amplitude", "[audio][synth]") {
    const auto samples = sine_blip(440.0F, 100, 0.25F);
    const auto [low, high] = std::ranges::minmax_element(samples);
    CHECK(std::abs(*low) <= 0.25F);
    CHECK(*high <= 0.25F);
    // And actually reaches it, so a fade that swallowed the whole sound would fail here
    // rather than passing as "within range".
    CHECK(*high > 0.2F);
}

TEST_CASE("a blip shorter than its fades still fades", "[audio][synth]") {
    // Two millisecond sound, five millisecond fades asked for. The fades are halved to fit
    // rather than clipped away, so the result is still silent at both ends.
    const auto samples = sine_blip(440.0F, 2, 0.5F, 5);
    REQUIRE(samples.size() > 2);
    CHECK(samples.front() == Approx(0.0F).margin(1e-6));
    CHECK(samples.back() == Approx(0.0F).margin(1e-6));
}

TEST_CASE("a zero-length blip is empty rather than an error", "[audio][synth]") {
    CHECK(sine_blip(440.0F, 0).empty());
}

TEST_CASE("absurd parameters are clamped, not refused", "[audio][synth]") {
    // Presentation values. A caller that computed a frequency from something else wants a
    // sound, not a failure it has to handle at every call site.
    CHECK_FALSE(sine_blip(-100.0F, 10).empty());
    CHECK_FALSE(sine_blip(1e9F, 10).empty());

    const auto loud = sine_blip(440.0F, 50, 100.0F);
    const auto [low, high] = std::ranges::minmax_element(loud);
    CHECK(*high <= 1.0F);
    CHECK(*low >= -1.0F);
}

TEST_CASE("the same request produces the same samples", "[audio][synth]") {
    // Not a determinism claim about the engine — audio is presentation and is hashed nowhere.
    // It is what lets the mixing benchmark compare two runs at all.
    CHECK(sine_blip(880.0F, 25) == sine_blip(880.0F, 25));
}

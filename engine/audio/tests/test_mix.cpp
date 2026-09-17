// SPDX-License-Identifier: GPL-3.0-or-later
// The mixer, with no device anywhere.
//
// This is where the arithmetic that can be wrong lives, so this is where it is pinned. Every
// case here is exact or within a stated tolerance, and none of it needs a sound card, which is
// the whole reason the mixer is a free function over buffers rather than a method on a device.
#include <atlas/audio/mix.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using atlas::audio::Bus;
using atlas::audio::clamp_block;
using atlas::audio::kMixChannels;
using atlas::audio::kMixSampleRate;
using atlas::audio::mix_voice;
using atlas::audio::pan_gains;
using atlas::audio::PcmClip;
using atlas::audio::resample;
using atlas::audio::VoiceState;
using Catch::Approx;

namespace {

/// A mono clip whose samples are the values given, so a test can read the output directly.
[[nodiscard]] std::shared_ptr<const PcmClip> mono(std::vector<float> samples) {
    auto clip = std::make_shared<PcmClip>();
    clip->channels = 1;
    clip->samples = std::move(samples);
    return clip;
}

[[nodiscard]] std::shared_ptr<const PcmClip> stereo(std::vector<float> interleaved) {
    auto clip = std::make_shared<PcmClip>();
    clip->channels = 2;
    clip->samples = std::move(interleaved);
    return clip;
}

/// A block of `frames` stereo frames, zeroed.
[[nodiscard]] std::vector<float> block(std::size_t frames) {
    // Parenthesised deliberately. The braced form the linter prefers selects the
    // initializer-list constructor instead, which builds a two-element vector holding the
    // count and the fill value rather than a block of zeros, and every test here would then
    // be reading past the end of it.
    // NOLINTNEXTLINE(modernize-return-braced-init-list)
    return std::vector<float>(frames * kMixChannels, 0.0F);
}

}  // namespace

TEST_CASE("a centred mono voice reaches both channels equally", "[audio][mix]") {
    VoiceState voice;
    voice.clip = mono({1.0F, 0.5F});
    auto out = block(2);

    CHECK(mix_voice(voice, out, 1.0F));

    // Constant-power pan puts a centred sound at about 0.707 per channel, not 1. That is the
    // trade the pan law makes and it is asserted here so a change to it fails visibly.
    CHECK(out[0] == Approx(0.70710678F).margin(1e-5));
    CHECK(out[1] == Approx(0.70710678F).margin(1e-5));
    CHECK(out[2] == Approx(0.35355339F).margin(1e-5));
    CHECK(out[3] == Approx(0.35355339F).margin(1e-5));
    CHECK(voice.cursor == 2);
}

TEST_CASE("two voices sum into one block", "[audio][mix]") {
    VoiceState a;
    a.clip = mono({0.25F});
    VoiceState b;
    b.clip = mono({0.25F});

    auto out = block(1);
    CHECK(mix_voice(a, out, 1.0F));
    const float after_one = out[0];
    CHECK(mix_voice(b, out, 1.0F));

    // Added, not replaced. A mixer that overwrote would pass every single-voice test above.
    CHECK(out[0] == Approx(after_one * 2.0F).margin(1e-6));
}

TEST_CASE("pan reaches the endpoints and stays constant-power", "[audio][mix]") {
    const auto left = pan_gains(-1.0F);
    CHECK(left.left == 1.0F);
    CHECK(left.right == Approx(0.0F).margin(1e-6));

    const auto right = pan_gains(1.0F);
    CHECK(right.left == Approx(0.0F).margin(1e-6));
    CHECK(right.right == Approx(1.0F).margin(1e-6));

    const auto centre = pan_gains(0.0F);
    CHECK(centre.left == Approx(0.70710678F).margin(1e-5));
    CHECK(centre.right == Approx(0.70710678F).margin(1e-5));

    // Constant power means the squares sum to one everywhere, which is the property that
    // stops a sound dipping as it crosses the middle.
    for (const float pan : {-1.0F, -0.5F, 0.0F, 0.25F, 1.0F}) {
        const auto gains = pan_gains(pan);
        CHECK((gains.left * gains.left) + (gains.right * gains.right) == Approx(1.0F).margin(1e-5));
    }

    // Out of range is clamped, because a pan computed from a position is presentation and an
    // error here would mean a caller had to validate a number it does not control.
    CHECK(pan_gains(-4.0F).left == pan_gains(-1.0F).left);
    CHECK(pan_gains(4.0F).right == pan_gains(1.0F).right);
}

TEST_CASE("a stereo source keeps its channels apart", "[audio][mix]") {
    VoiceState voice;
    // Left is one and right is zero in the first frame, and the other way round in the second.
    // A mixer that duplicated the source's first channel — which is what it must do for a mono
    // clip and must not do for a stereo one — would put the same value in both outputs.
    voice.clip = stereo({1.0F, 0.0F, 0.0F, 1.0F});
    // Centred on purpose. An earlier version of this test panned hard left, which zeroes the
    // right output channel outright and so cannot see the fold at all: it passed against a
    // mixer that ignored the source's second channel entirely. Mutation testing found that;
    // the assertions below did not.
    voice.pan = 0.0F;
    auto out = block(2);

    constexpr float kCentre = 0.70710678F;
    CHECK(mix_voice(voice, out, 1.0F));
    CHECK(out[0] == Approx(kCentre).margin(1e-5));
    CHECK(out[1] == Approx(0.0F).margin(1e-6));
    CHECK(out[2] == Approx(0.0F).margin(1e-6));
    CHECK(out[3] == Approx(kCentre).margin(1e-5));
}

TEST_CASE("a one-shot ends exactly at the end of its clip", "[audio][mix]") {
    VoiceState voice;
    voice.clip = mono({1.0F, 1.0F});
    auto out = block(4);

    // Four frames asked for, two available: the voice reports that it finished, and the two
    // frames past the end are silent rather than repeating the last sample.
    CHECK_FALSE(mix_voice(voice, out, 1.0F));
    CHECK(out[4] == 0.0F);
    CHECK(out[5] == 0.0F);
}

TEST_CASE("a looping voice wraps and never finishes", "[audio][mix]") {
    VoiceState voice;
    voice.clip = mono({1.0F, 2.0F});
    voice.loop = true;
    auto out = block(5);

    CHECK(mix_voice(voice, out, 1.0F));

    // The wrap is seamless: frame 2 is the clip's frame 0 again, at the same value, with no
    // gap and no repeated sample at the join.
    const float first = out[0];
    const float second = out[2];
    CHECK(out[4] == Approx(first).margin(1e-6));
    CHECK(out[6] == Approx(second).margin(1e-6));
    CHECK(out[8] == Approx(first).margin(1e-6));

    // The cursor wraps rather than running away, so an hour of looping is still sample-exact.
    CHECK(voice.cursor == 1);
}

TEST_CASE("a voice with nothing to play finishes immediately", "[audio][mix]") {
    auto out = block(4);

    VoiceState empty;
    CHECK_FALSE(mix_voice(empty, out, 1.0F));

    VoiceState zero_length;
    zero_length.clip = mono({});
    zero_length.loop = true;  // even looping, so it cannot spin forever on an empty clip
    CHECK_FALSE(mix_voice(zero_length, out, 1.0F));
}

TEST_CASE("gain and volume both scale the voice", "[audio][mix]") {
    VoiceState voice;
    voice.clip = mono({1.0F});
    voice.volume = 0.5F;
    voice.pan = -1.0F;
    auto out = block(1);

    CHECK(mix_voice(voice, out, 0.5F));
    CHECK(out[0] == Approx(0.25F).margin(1e-6));
}

TEST_CASE("the block is clamped once, at the end", "[audio][mix]") {
    std::vector<float> out{2.0F, -2.0F, 0.5F, -0.5F};
    clamp_block(out);
    CHECK(out[0] == 1.0F);
    CHECK(out[1] == -1.0F);
    CHECK(out[2] == 0.5F);
    CHECK(out[3] == -0.5F);
}

TEST_CASE("resampling is a no-op at the mix rate", "[audio][mix]") {
    const std::vector<float> input{0.0F, 0.25F, 0.5F, 1.0F};
    const auto output = resample(input, 1, kMixSampleRate);
    // Byte-identical, because the common case must not pass through the interpolator at all.
    CHECK(output == input);
}

TEST_CASE("resampling doubles the frames when the rate halves", "[audio][mix]") {
    const std::vector<float> input{0.0F, 1.0F, 0.0F, 1.0F};
    const auto output = resample(input, 1, kMixSampleRate / 2);

    CHECK(output.size() == 8);
    // The original frames land on even positions and the interpolated ones sit halfway.
    CHECK(output[0] == Approx(0.0F).margin(1e-6));
    CHECK(output[1] == Approx(0.5F).margin(1e-6));
    CHECK(output[2] == Approx(1.0F).margin(1e-6));
    CHECK(output[3] == Approx(0.5F).margin(1e-6));
}

TEST_CASE("resampling keeps stereo channels apart", "[audio][mix]") {
    // Left is all zero, right is all one. If the interpolator ever read across the channel
    // stride, the two would bleed into each other and this is what would show it.
    const std::vector<float> input{0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F};
    const auto output = resample(input, 2, kMixSampleRate / 2);

    REQUIRE(output.size() % 2 == 0);
    for (std::size_t frame = 0; frame < output.size() / 2; ++frame) {
        CHECK(output[frame * 2] == Approx(0.0F).margin(1e-6));
        CHECK(output[(frame * 2) + 1] == Approx(1.0F).margin(1e-6));
    }
}

TEST_CASE("resampling refuses what it cannot divide by", "[audio][mix]") {
    const std::vector<float> input{1.0F, 1.0F};
    CHECK(resample(input, 1, 0).empty());
    CHECK(resample(input, 0, 44'100).empty());
    CHECK(resample({}, 1, 44'100).empty());
}

TEST_CASE("every bus has a name", "[audio][mix]") {
    CHECK(atlas::audio::to_string(Bus::Master) == "master");
    CHECK(atlas::audio::to_string(Bus::Music) == "music");
    CHECK(atlas::audio::to_string(Bus::Effects) == "effects");
}

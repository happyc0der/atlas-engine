// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/audio/mix.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace atlas::audio {

std::string_view to_string(Bus bus) noexcept {
    switch (bus) {
    case Bus::Master: return "master";
    case Bus::Music: return "music";
    case Bus::Effects: return "effects";
    case Bus::Count: break;
    }
    return "unrecognised";
}

PanGains pan_gains(float pan) noexcept {
    const float clamped = std::clamp(pan, -1.0F, 1.0F);
    // Map [-1, 1] onto a quarter turn, then take the two legs. At -1 the angle is exactly
    // zero, so hard left is exactly {1, 0}; at +1 the cosine is a rounding error away from
    // zero rather than exactly it, which is why the tests compare hard right with a tolerance.
    const float angle = (clamped + 1.0F) * (std::numbers::pi_v<float> / 4.0F);
    return PanGains{.left = std::cos(angle), .right = std::sin(angle)};
}

bool mix_voice(VoiceState& voice, std::span<float> out, float gain) noexcept {
    if (!voice.clip || voice.clip->channels == 0 || voice.clip->samples.empty()) {
        return false;
    }

    const PcmClip& clip = *voice.clip;
    const std::size_t clip_frames = clip.frames();
    if (clip_frames == 0) {
        return false;
    }

    const PanGains pan = pan_gains(voice.pan);
    const float left_gain = gain * voice.volume * pan.left;
    const float right_gain = gain * voice.volume * pan.right;
    const bool stereo_source = clip.channels >= 2;

    const std::size_t out_frames = out.size() / kMixChannels;
    for (std::size_t frame = 0; frame < out_frames; ++frame) {
        if (voice.cursor >= clip_frames) {
            if (!voice.loop) {
                return false;
            }
            // The cursor is checked once per output frame and advances by one, so it can
            // only ever arrive exactly at the end: zero is the whole of the wrap, and
            // subtracting the length instead would be the same statement written longer.
            // An earlier comment here claimed this was protecting against drift over a long
            // loop. It was not, and a mutation to the arithmetic proved it by surviving.
            voice.cursor = 0;
        }

        const std::size_t base = voice.cursor * clip.channels;
        const float left = clip.samples[base];
        const float right = stereo_source ? clip.samples[base + 1] : left;

        out[frame * kMixChannels] += left * left_gain;
        out[(frame * kMixChannels) + 1] += right * right_gain;
        ++voice.cursor;
    }

    return true;
}

void clamp_block(std::span<float> out) noexcept {
    for (float& sample : out) {
        sample = std::clamp(sample, -1.0F, 1.0F);
    }
}

std::vector<float> resample(std::span<const float> input, std::uint32_t channels,
                            std::uint32_t source_rate) {
    if (input.empty() || channels == 0 || source_rate == 0) {
        return {};
    }
    if (source_rate == kMixSampleRate) {
        return std::vector<float>{input.begin(), input.end()};
    }

    const std::size_t source_frames = input.size() / channels;
    if (source_frames == 0) {
        return {};
    }

    // Round rather than truncate, so a rate that does not divide evenly does not lose the
    // final fraction of a frame and leave a click where the clip should have ended.
    const double ratio = static_cast<double>(kMixSampleRate) / static_cast<double>(source_rate);
    const auto target_frames =
        static_cast<std::size_t>(std::llround(static_cast<double>(source_frames) * ratio));
    if (target_frames == 0) {
        return {};
    }

    std::vector<float> output(target_frames * channels);
    for (std::size_t frame = 0; frame < target_frames; ++frame) {
        const double position = static_cast<double>(frame) / ratio;
        const auto lower = static_cast<std::size_t>(position);
        // The last output frame can land exactly on, or a rounding step past, the last input
        // frame. Holding the final sample there is what stops the interpolation reading off
        // the end; the alternative, refusing the frame, would shorten the clip instead.
        const std::size_t upper = std::min(lower + 1, source_frames - 1);
        const auto fraction = static_cast<float>(position - static_cast<double>(lower));

        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const float a = input[(std::min(lower, source_frames - 1) * channels) + channel];
            const float b = input[(upper * channels) + channel];
            output[(frame * channels) + channel] = a + ((b - a) * fraction);
        }
    }
    return output;
}

}  // namespace atlas::audio

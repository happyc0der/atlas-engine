// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/audio/synth.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace atlas::audio {

std::vector<float> sine_blip(float frequency_hz, std::uint32_t duration_ms, float amplitude,
                             std::uint32_t fade_ms) {
    const float frequency = std::clamp(frequency_hz, 1.0F, 20'000.0F);
    const float level = std::clamp(amplitude, 0.0F, 1.0F);
    const std::uint32_t duration = std::min(duration_ms, 60'000U);

    const auto frames =
        static_cast<std::size_t>((static_cast<std::uint64_t>(kMixSampleRate) * duration) / 1000);
    if (frames == 0) {
        return {};
    }

    // Two fades cannot together be longer than the sound. Halving keeps them symmetric, which
    // is what a caller asking for a fade on a very short blip actually wants.
    const auto requested_fade =
        static_cast<std::size_t>((static_cast<std::uint64_t>(kMixSampleRate) * fade_ms) / 1000);
    const std::size_t fade = std::min(requested_fade, frames / 2);

    std::vector<float> samples(frames);
    const float step =
        2.0F * std::numbers::pi_v<float> * frequency / static_cast<float>(kMixSampleRate);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float envelope = 1.0F;
        if (fade > 0) {
            if (frame < fade) {
                envelope = static_cast<float>(frame) / static_cast<float>(fade);
            } else if (frame >= frames - fade) {
                envelope = static_cast<float>(frames - 1 - frame) / static_cast<float>(fade);
            }
        }
        samples[frame] = std::sin(step * static_cast<float>(frame)) * level * envelope;
    }
    return samples;
}

}  // namespace atlas::audio

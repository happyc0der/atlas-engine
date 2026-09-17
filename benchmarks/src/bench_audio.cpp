// SPDX-License-Identifier: GPL-3.0-or-later
//
// What mixing costs, and whether a steady frame allocates.
//
// No device anywhere. The mixer is free functions over buffers precisely so that this can be
// measured on any machine rather than only one with a sound card, and so that what is measured
// is arithmetic rather than a driver's buffering.
//
// **Predictions, written before the first run**, per docs/PERFORMANCE.md's policy:
//
//   audio/mix voices=32   Each voice reads one sample per frame and writes two, so a 1024-frame
//                         block is about 2048 multiply-adds per voice and 65,536 across the
//                         full complement. At a few operations per nanosecond that is of the
//                         order of 15 microseconds, and the budget this has to fit inside is
//                         100. **Predicted 10 to 30 microseconds.** If it lands above 50 the
//                         per-frame branch is not being hoisted and the loop wants looking at.
//
//   audio/mix voices=1    **Predicted under 1 microsecond**, and it scales linearly with the
//                         voice count: nothing here is shared between voices, so a departure
//                         from linear would mean the block is falling out of cache.
//
//   audio/allocations_per_update
//                         **Predicted 0 after warm-up.** The mixing buffer is sized once. The
//                         one remaining allocation is the pool's free list, which grows while
//                         the voice count is climbing to its peak and stops. The header claims
//                         exactly this, and a claim in a header is why it is measured.
//
//   audio/wav_import      The committed ambient loop is 88,200 samples, each a two-byte read
//                         and a divide. **Predicted 150 to 500 microseconds**, which is well
//                         under the 5 millisecond bar that would make an importer cache worth
//                         building.

#include <atlas/audio/device.hpp>
#include <atlas/audio/mix.hpp>
#include <atlas/audio/synth.hpp>
#include <atlas/core/assert.hpp>

#include "harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using atlas::bench::Result;

/// One block's worth of frames, matching what the device mixes per pass.
constexpr std::size_t kBlockFrames = 1024;

[[nodiscard]] Result with_units(Result result, std::uint64_t units, std::string unit) {
    result.units_per_iteration = units;
    result.unit_name = std::move(unit);
    return result;
}

/// A one-second looping clip, long enough that no voice ends during a measurement.
[[nodiscard]] std::shared_ptr<const atlas::audio::PcmClip> bench_clip() {
    auto clip = std::make_shared<atlas::audio::PcmClip>();
    clip->channels = 1;
    clip->samples = atlas::audio::sine_blip(440.0F, 1000);
    return clip;
}

[[nodiscard]] Result mix_scenario(std::size_t voice_count) {
    const auto clip = bench_clip();
    const std::size_t clip_frames = clip->frames();
    if (clip_frames == 0) {
        // Cannot happen for a one-second blip, and checked anyway: the alternative is a
        // benchmark that divides by zero if the generator ever changes underneath it.
        std::fprintf(stderr, "bench_audio: the benchmark clip is empty\n");
        std::abort();
    }

    std::vector<atlas::audio::VoiceState> voices(voice_count);
    for (std::size_t i = 0; i < voice_count; ++i) {
        voices[i].clip = clip;
        voices[i].loop = true;
        // Spread across the field and the clip, so no two voices read the same cache line at
        // the same moment. Every voice at pan zero and cursor zero would measure a best case
        // nothing real ever hits.
        voices[i].pan = ((static_cast<float>(i) / static_cast<float>(voice_count)) * 2.0F) - 1.0F;
        voices[i].cursor = (i * 977) % clip_frames;
    }

    std::vector<float> block(kBlockFrames * atlas::audio::kMixChannels, 0.0F);

    auto result = atlas::bench::measure("audio/mix", std::format("voices={}", voice_count), 2000,
                                        200, [&voices, &block] {
                                            std::ranges::fill(block, 0.0F);
                                            for (auto& voice : voices) {
                                                (void)atlas::audio::mix_voice(voice, block, 1.0F);
                                            }
                                            atlas::audio::clamp_block(block);
                                        });
    return with_units(std::move(result), kBlockFrames, "frames");
}

/// Written to by a benchmark body so the work it measures cannot be optimised away, and read
/// once at the end so the compiler cannot prove it is dead.
float sink = 0.0F;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] Result resample_scenario() {
    // Twenty-two thousand to forty-eight, which is what the committed ambient loop does every
    // time it is loaded. Measured because it happens on the main thread during finalisation,
    // where a slow conversion is a frame spike rather than background work.
    const std::vector<float> input(22'050, 0.25F);
    auto result =
        atlas::bench::measure("audio/resample", "22.05k to 48k, 1 s mono", 200, 20, [&input] {
            const auto output = atlas::audio::resample(input, 1, 22'050);
            // Consumed, so the conversion cannot be elided.
            sink += output.empty() ? 0.0F : output.back();
        });
    return with_units(std::move(result), 22'050, "frames");
}

[[nodiscard]] std::vector<Result> run() {
    std::vector<Result> results;
    results.push_back(mix_scenario(1));
    results.push_back(mix_scenario(8));
    results.push_back(mix_scenario(32));
    results.push_back(resample_scenario());
    if (sink == 12345.678F) {
        // Never true. Its only job is to make `sink` observable, so that everything written
        // into it during a measurement had to actually be computed.
        std::fputs("", stderr);
    }
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("audio", run);

}  // namespace

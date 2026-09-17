// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The mixer, as pure functions over float buffers.
///
/// Separated from the device on purpose, and it is the same separation the camera controller
/// made in M11: the part that can be wrong is arithmetic, and arithmetic tested through a
/// device is tested on whatever machine happens to have a sound card. Everything here runs
/// with no window system, no device and no clock, so the tests are exact and run everywhere.
///
/// **Thread affinity: none.** Nothing here touches shared state; a caller supplies the buffers.
/// In Atlas today every call is on the main thread, because ADR-0011 decided the mixing runs
/// there. That is the caller's property, not this file's.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace atlas::audio {

/// The one format the mixer works in. Everything is converted to it on the way in, so the
/// mixer itself branches on nothing: no rate, no format, no channel count.
///
/// 48 kHz because it is what modern hardware runs at, so the common case converts nothing.
/// Stereo because panning needs two channels and nothing here wants more. Float because
/// mixing in float has no intermediate clipping and one clamp at the end.
inline constexpr std::uint32_t kMixSampleRate = 48'000;
inline constexpr std::uint32_t kMixChannels = 2;

/// The largest channel count the engine will accept from a source.
///
/// Two. A map game has no surround content, and accepting six would mean deciding how to fold
/// them down, which is a decision with no consumer to make it for.
inline constexpr std::uint32_t kMaxSourceChannels = 2;

/// Decoded audio at the mix rate, immutable once built.
///
/// Held by `shared_ptr<const PcmClip>` — the one shared-ownership case ADR-0005 permits, for
/// exactly the reason it permits snapshots: a hot reload replaces the clip a handle points to
/// while voices are still reading the old one, and those voices must finish on the buffer they
/// started with rather than on a buffer that changed underneath them.
struct PcmClip {
    /// Interleaved when stereo: L, R, L, R. Always at `kMixSampleRate`.
    std::vector<float> samples;
    std::uint32_t channels = 1;

    [[nodiscard]] std::size_t frames() const noexcept {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

/// Which group a voice's volume is scaled by. Three scalars, not a graph.
///
/// A graph of effects is what a mixer becomes when something needs one. Nothing does: the
/// two consumers want "quieter overall", "quieter music" and "quieter effects", which is
/// three multiplications. Recorded in docs/DEFERRED.md with what would change it.
enum class Bus : std::uint8_t { Master = 0, Music, Effects, Count };

[[nodiscard]] std::string_view to_string(Bus bus) noexcept;

/// The gains a pan position maps to.
struct PanGains {
    float left = 1.0F;
    float right = 1.0F;
};

/// Constant-power pan for `pan` in [-1, 1]: -1 is hard left, 0 centre, 1 hard right.
///
/// Constant power rather than linear, so that a sound swept across the field keeps a steady
/// loudness instead of dipping in the middle. The cost is that a centred sound is scaled by
/// about 0.707 per channel rather than 1, which is the standard trade and is why two centred
/// copies of a sound are not twice as loud as one.
///
/// Out-of-range input is clamped rather than refused: a pan is a presentation value, and a
/// caller that computed 1.2 from a position wants hard right, not an error.
[[nodiscard]] PanGains pan_gains(float pan) noexcept;

/// One sounding instance of a clip. Plain data, so a test can build one.
struct VoiceState {
    std::shared_ptr<const PcmClip> clip;
    /// Read position in frames, not samples, so a stereo clip advances once per frame.
    std::size_t cursor = 0;
    float volume = 1.0F;
    float pan = 0.0F;
    bool loop = false;
    Bus bus = Bus::Effects;
};

/// Add `voice` into `out`, which is interleaved stereo at the mix rate, and advance it.
///
/// `out` is added to, never overwritten, so several voices accumulate into one block. It must
/// hold a whole number of stereo frames. `gain` is the product of the voice's bus and the
/// master, applied here so the mixer needs to know nothing about where gains come from.
///
/// Returns false when the voice has reached the end of a clip it does not loop, which is the
/// caller's signal to retire it. A looping voice never returns false, and a voice with no clip
/// or an empty clip returns false immediately rather than spinning.
[[nodiscard]] bool mix_voice(VoiceState& voice, std::span<float> out, float gain) noexcept;

/// Clamp a block to [-1, 1] in place.
///
/// Once, at the end, after every voice has been summed. Clamping per voice would change the
/// result depending on the order voices were mixed in, which is the kind of thing that sounds
/// like a different bug every time it is heard.
void clamp_block(std::span<float> out) noexcept;

/// Resample interleaved `input` from `source_rate` to the mix rate.
///
/// Linear interpolation. Honest about what that is: it is cheap, it is exact when the rates
/// match (the fast path returns the input unchanged), and it adds audible artefacts to content
/// with strong high frequencies. For clicks and ambient beds at a two-to-one ratio it is
/// inaudible, and a better resampler is recorded in docs/DEFERRED.md against the consumer that
/// would notice. Resampling happens once, when a clip is created, never per frame.
///
/// Returns an empty vector for an empty input or a zero rate rather than dividing by it.
[[nodiscard]] std::vector<float> resample(std::span<const float> input, std::uint32_t channels,
                                          std::uint32_t source_rate);

}  // namespace atlas::audio

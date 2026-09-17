// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The output device, the clips it holds, and the voices sounding on it.
///
/// **Ownership and lifetime.** Move-only. It must be destroyed **before** `platform::Platform`,
/// whose destructor shuts down every window-system subsystem at once, including the audio one
/// this device's stream belongs to. Declaring it after the platform in the same scope gives
/// that for free, which is how both applications do it and how the graphics device already
/// works.
///
/// **Thread affinity: the main thread, throughout.** ADR-0011 chose push mixing on the main
/// thread over a callback on the window system's audio thread, so no Atlas code ever runs on
/// that thread and nothing here needs to be safe against it. The cost is that a frame longer
/// than the queued audio can be heard as a gap; `AudioStats::underruns` counts exactly that,
/// and the ADR records the budget and the trigger for revisiting it.
///
/// **Failure behaviour.** `create` fails when there is no device; it never reports success for
/// something that will not make a sound. An application that wants to carry on anyway asks for
/// `null()` and logs that it did, so "there is no audio" is a decision in the application's
/// log rather than a silence nobody can explain.
///
/// No window-system type appears anywhere in this header.

#include <atlas/assets/asset_id.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/audio/mix.hpp>
#include <atlas/core/handle.hpp>
#include <atlas/core/result.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace atlas::audio {

struct ClipTag;
struct VoiceTag;

/// A loaded clip. Generation-counted, so a handle to a clip that has been replaced or released
/// resolves to nothing instead of to whatever now occupies the slot.
using ClipHandle = Handle<ClipTag>;

/// One sounding instance. Expected to go stale: a one-shot retires itself when it ends, and a
/// caller holding its handle afterwards gets `false` from `stop`, not a surprise.
using VoiceHandle = Handle<VoiceTag>;

/// What a clip is made of, on the way in.
struct ClipDesc {
    /// Interleaved if `channels` is 2. Copied, resampled if needed, and not referenced after
    /// `create_clip` returns.
    std::span<const float> samples;
    std::uint32_t channels = 1;
    std::uint32_t sample_rate = kMixSampleRate;
    /// For the log line when something is refused. Not stored.
    std::string_view debug_name;
};

struct PlayParams {
    float volume = 1.0F;
    /// -1 hard left, 0 centre, 1 hard right. Clamped, not refused.
    float pan = 0.0F;
    bool loop = false;
    Bus bus = Bus::Effects;
};

struct AudioConfig {
    /// How much audio to keep ahead of the device, in milliseconds.
    ///
    /// The whole latency argument lives in this number. Sixty milliseconds plus the device's
    /// own buffer absorbs any frame shorter than sixty milliseconds; the worst frame this
    /// project has measured is twenty-three. Raising it buys protection from long frames and
    /// costs responsiveness one-for-one. See ADR-0011.
    std::uint32_t queue_target_ms = 60;
    /// Never push past this. Stops a stall followed by a catch-up from queueing seconds of
    /// audio that then has to play out before anything new is heard.
    std::uint32_t queue_max_ms = 120;
    /// Voices sounding at once. A refused play returns a null handle and is counted.
    std::uint32_t max_voices = 32;
};

struct AudioStats {
    std::uint32_t voices = 0;
    std::uint32_t voices_peak = 0;
    std::uint32_t clips = 0;
    /// Milliseconds of audio the device has accepted and not yet played.
    std::uint32_t queued_ms = 0;
    /// Times `update` found the queue empty while voices were still sounding. Each one is
    /// audible as a gap.
    std::uint64_t underruns = 0;
    std::uint64_t plays_refused = 0;
    bool null_device = false;
};

class AudioDevice {
  public:
    /// Open the default output device.
    ///
    /// Failure: `AudioInitFailed` when the subsystem was never started — which is what happens
    /// when the composition root did not ask the platform for it — and
    /// `AudioDeviceUnavailable` when there is no device or it refuses to open.
    [[nodiscard]] static Result<AudioDevice> create(const AudioConfig& config = {});

    /// A device that accepts everything and plays nothing.
    ///
    /// Deliberately not what `create` falls back to. A silent fallback inside `create` would
    /// mean no caller could tell a working device from a missing one, and the first symptom
    /// would be a bug report saying the game has no sound. Voices still start, advance in real
    /// time and retire, so voice counts and clip lifetimes behave as they would with a device
    /// and a headless run settles the same state.
    [[nodiscard]] static AudioDevice null(const AudioConfig& config = {});

    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    AudioDevice(AudioDevice&& other) noexcept;
    AudioDevice& operator=(AudioDevice&& other) noexcept;

    /// Copy, convert and resample audio into a clip.
    ///
    /// Failure: `AudioFormatUnsupported` for zero channels, more than `kMaxSourceChannels`, or
    /// a sample rate outside what a decoder could plausibly produce; `InvalidArgument` for a
    /// sample count that is not a whole number of frames; `Exhausted` when the clip pool is
    /// full.
    [[nodiscard]] Result<ClipHandle> create_clip(const ClipDesc& desc);

    /// Release a clip. Voices already sounding on it finish on the buffer they started with.
    bool destroy_clip(ClipHandle clip);

    /// Turn every decoded audio asset into a clip. Once per frame, on the main thread.
    ///
    /// The same shape as the texture cache's own finaliser and for the same reason: workers
    /// produce samples and stop there, and the main-thread work — resampling to the mix rate
    /// and inserting into the pool — belongs to whoever owns the pool. Assets of other types
    /// are skipped, so several finalisers share one registry without stepping on each other.
    ///
    /// Returns how many clips were created, which for a steady frame is zero. A clip that
    /// cannot be created is marked failed in the registry with the reason, never retried in a
    /// loop, and never allowed to stop the frame.
    ///
    /// The null device finalises too. A run with no sound still settles the asset state
    /// machine, so "there was no audio device" never becomes "assets are stuck".
    std::size_t finalise_pending(assets::Registry& registry);

    /// The clip for an asset, or a null handle if it is missing, failed, or not yet ready.
    ///
    /// Deliberately no fallback sound. A texture resolves to a magenta checkerboard because a
    /// missing texture must still draw something; a missing sound has nothing it must still
    /// do, and inventing a noise would be worse than silence. The registry says which of the
    /// three it is, and the asset panel shows it.
    [[nodiscard]] ClipHandle clip_for(assets::AssetId id) const;

    /// Start a voice.
    ///
    /// Returns a null handle when the clip is unknown or the voice limit is reached, rather
    /// than a `Result`: refusing to play a sound is a normal, expected outcome under load and
    /// forcing every call site to handle an error would mean every call site ignored it. The
    /// refusal is counted in the statistics and logged once.
    [[nodiscard]] VoiceHandle play(ClipHandle clip, const PlayParams& params = {});

    bool stop(VoiceHandle voice);
    bool set_volume(VoiceHandle voice, float volume);
    void stop_all();

    /// Scale everything on a bus. Clamped to [0, 4]: above one is a boost, which is useful and
    /// is not a reason to allow an arbitrary multiplier into the mix.
    void set_bus_volume(Bus bus, float volume);
    [[nodiscard]] float bus_volume(Bus bus) const noexcept;

    /// Mix and push whatever is needed to reach the queue target. Once per frame.
    ///
    /// The mixing buffer is sized at creation and never grows. One allocation remains, and it
    /// is named here rather than glossed: retiring a voice records the freed slot in the
    /// pool's free list, which grows until it has held as many entries as there have been
    /// simultaneous voices. So this is allocation-free once the voice count has peaked, not
    /// from the first call. M12's benchmark measures it rather than trusting this sentence.
    void update();

    [[nodiscard]] AudioStats stats() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;

  private:
    AudioDevice();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::audio

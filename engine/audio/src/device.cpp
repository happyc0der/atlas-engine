// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/audio/device.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/core/time.hpp>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace atlas::audio {
namespace {

constexpr log::Category kAudio{"audio"};

/// Frames mixed per pass. About 21 ms at the mix rate, so reaching a 60 ms target takes three
/// passes from empty and usually one in the steady state. Small enough that the buffer is a
/// few kilobytes and large enough that the per-pass overhead is nothing.
constexpr std::size_t kBlockFrames = 1024;

/// Passes `update` will run before giving up for this frame.
///
/// A ceiling on work, not on correctness: reaching it means the queue is further behind than
/// one frame should ever have to make up, and mixing forever to catch up would turn a stall
/// into a longer stall. Eight passes is about 170 ms, comfortably past `queue_max_ms`.
constexpr int kMaxPassesPerUpdate = 8;

/// What a decoder could plausibly produce. Outside this, the file is lying about itself.
constexpr std::uint32_t kMinSampleRate = 8'000;
constexpr std::uint32_t kMaxSampleRate = 192'000;

/// The window system's "default playback device" constant.
///
/// Named here because the macro that defines it contains a C-style cast, which first-party
/// code compiles with as an error. Confining the suppression to one line is better than
/// widening the warning for the whole file, and better than reproducing the literal, which
/// would silently diverge if the library ever changed it.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
constexpr SDL_AudioDeviceID kDefaultPlaybackDevice = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

[[nodiscard]] std::uint32_t frames_to_ms(std::size_t frames) noexcept {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(frames) * 1000) / kMixSampleRate);
}

[[nodiscard]] std::size_t ms_to_frames(std::uint32_t milliseconds) noexcept {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(milliseconds) * kMixSampleRate) /
                                    1000);
}

}  // namespace

struct AudioDevice::Impl {
    AudioConfig config;

    /// The window system's stream, or null for a null device. A `void*` for the same reason
    /// the platform stores a gamepad handle as one: the type may not appear in a header.
    void* stream = nullptr;

    HandlePool<std::shared_ptr<const PcmClip>, ClipTag> clips;
    HandlePool<VoiceState, VoiceTag> voices;

    std::array<float, static_cast<std::size_t>(Bus::Count)> bus_volume{1.0F, 1.0F, 1.0F};

    /// Sized once, in create. `update` never grows it.
    std::vector<float> block;

    /// For the null device: where real time was when it last advanced. The null device has no
    /// queue to measure, so it advances by the clock instead, which is what keeps a voice's
    /// lifetime the same length with and without a device.
    SteadyClock clock;
    bool advanced_once = false;

    std::uint32_t voices_peak = 0;
    std::uint64_t underruns = 0;
    std::uint64_t plays_refused = 0;
    bool warned_refusal = false;
    bool warned_underrun = false;

    /// Mix one block's worth of `frames` into `block` and retire anything that finished.
    void mix_into_block(std::size_t frames) {
        const std::size_t samples = frames * kMixChannels;
        std::fill(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(samples), 0.0F);
        const std::span<float> out{block.data(), samples};

        const float master = bus_volume[static_cast<std::size_t>(Bus::Master)];

        // Collected first, then destroyed: retiring a voice inside for_each_live would mutate
        // the pool while walking it.
        std::array<VoiceHandle, 64> finished{};
        std::size_t finished_count = 0;

        voices.for_each_live([&](VoiceHandle handle, VoiceState& voice) {
            const float gain = master * bus_volume[static_cast<std::size_t>(voice.bus)];
            if (!mix_voice(voice, out, gain) && finished_count < finished.size()) {
                finished[finished_count] = handle;
                ++finished_count;
            }
        });

        for (std::size_t i = 0; i < finished_count; ++i) {
            voices.destroy(finished[i]);
        }

        clamp_block(out);
    }
};

AudioDevice::AudioDevice() : m_impl(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
    if (m_impl && m_impl->stream != nullptr) {
        // Destroying the stream also closes the device it was opened on. Silent, like the
        // platform's destructor and for the same reason: formatting allocates.
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(m_impl->stream));
        m_impl->stream = nullptr;
    }
}

AudioDevice::AudioDevice(AudioDevice&& other) noexcept = default;

AudioDevice& AudioDevice::operator=(AudioDevice&& other) noexcept {
    if (this != &other) {
        // The stream this device holds has to go before the pointer is overwritten, or the
        // device stays open for the life of the process with nothing referring to it.
        if (m_impl && m_impl->stream != nullptr) {
            SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(m_impl->stream));
            m_impl->stream = nullptr;
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Result<AudioDevice> AudioDevice::create(const AudioConfig& config) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        return std::unexpected(
            Error(ErrorCode::AudioInitFailed,
                  "the audio subsystem is not running; the platform was created without "
                  "PlatformConfig::audio"));
    }

    AudioDevice device;
    device.m_impl->config = config;
    device.m_impl->config.queue_max_ms = std::max(config.queue_max_ms, config.queue_target_ms);

    const SDL_AudioSpec spec{
        .format = SDL_AUDIO_F32,
        .channels = static_cast<int>(kMixChannels),
        .freq = static_cast<int>(kMixSampleRate),
    };

    // No callback. With none registered the window system's own audio thread drains the
    // stream and never enters Atlas code, which is the whole of ADR-0011's threading claim.
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(kDefaultPlaybackDevice, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        return std::unexpected(
            Error(ErrorCode::AudioDeviceUnavailable,
                  std::format("opening the default audio device failed: {}", SDL_GetError())));
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        SDL_DestroyAudioStream(stream);
        return std::unexpected(
            Error(ErrorCode::AudioDeviceUnavailable,
                  std::format("the audio device opened but would not start: {}", SDL_GetError())));
    }

    device.m_impl->stream = stream;
    device.m_impl->block.assign(kBlockFrames * kMixChannels, 0.0F);

    const char* driver = SDL_GetCurrentAudioDriver();
    ATLAS_LOG_INFO(kAudio, "audio ready: driver '{}', {} Hz stereo float, {} ms target",
                   driver != nullptr ? driver : "?", kMixSampleRate, config.queue_target_ms);
    return device;
}

AudioDevice AudioDevice::null(const AudioConfig& config) {
    AudioDevice device;
    device.m_impl->config = config;
    device.m_impl->config.queue_max_ms = std::max(config.queue_max_ms, config.queue_target_ms);
    device.m_impl->block.assign(kBlockFrames * kMixChannels, 0.0F);
    return device;
}

bool AudioDevice::is_null() const noexcept {
    return !m_impl || m_impl->stream == nullptr;
}

Result<ClipHandle> AudioDevice::create_clip(const ClipDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT(m_impl != nullptr);

    if (desc.channels == 0 || desc.channels > kMaxSourceChannels) {
        return std::unexpected(
            Error(ErrorCode::AudioFormatUnsupported,
                  std::format("'{}' has {} channels; the engine mixes mono and stereo only",
                              desc.debug_name, desc.channels)));
    }
    if (desc.sample_rate < kMinSampleRate || desc.sample_rate > kMaxSampleRate) {
        return std::unexpected(
            Error(ErrorCode::AudioFormatUnsupported,
                  std::format("'{}' claims {} Hz, outside [{}, {}]", desc.debug_name,
                              desc.sample_rate, kMinSampleRate, kMaxSampleRate)));
    }
    if (desc.samples.size() % desc.channels != 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("'{}' has {} samples, which is not a whole number of {}-channel "
                              "frames",
                              desc.debug_name, desc.samples.size(), desc.channels)));
    }

    auto clip = std::make_shared<PcmClip>();
    clip->channels = desc.channels;
    clip->samples = resample(desc.samples, desc.channels, desc.sample_rate);

    auto handle = m_impl->clips.insert(std::shared_ptr<const PcmClip>{std::move(clip)});
    if (!handle) {
        return std::unexpected(std::move(handle).error().context(
            std::format("creating a clip for '{}'", desc.debug_name)));
    }
    return *handle;
}

bool AudioDevice::destroy_clip(ClipHandle clip) {
    ATLAS_ASSERT_MAIN_THREAD();
    // Voices keep their own shared_ptr, so anything still sounding finishes on the buffer it
    // started with rather than reading a slot that has been reused.
    return m_impl && m_impl->clips.destroy(clip);
}

VoiceHandle AudioDevice::play(ClipHandle clip, const PlayParams& params) {
    ATLAS_ASSERT_MAIN_THREAD();
    if (!m_impl) {
        return {};
    }

    const auto* stored = m_impl->clips.get(clip);
    if (stored == nullptr || !*stored) {
        return {};
    }

    if (m_impl->voices.size() >= m_impl->config.max_voices) {
        ++m_impl->plays_refused;
        if (!m_impl->warned_refusal) {
            m_impl->warned_refusal = true;
            ATLAS_LOG_WARN(kAudio,
                           "the voice limit of {} was reached and a sound was dropped; further "
                           "refusals are counted but not logged",
                           m_impl->config.max_voices);
        }
        return {};
    }

    VoiceState voice;
    voice.clip = *stored;
    voice.volume = std::clamp(params.volume, 0.0F, 4.0F);
    voice.pan = std::clamp(params.pan, -1.0F, 1.0F);
    voice.loop = params.loop;
    voice.bus = params.bus == Bus::Count ? Bus::Effects : params.bus;

    auto handle = m_impl->voices.insert(std::move(voice));
    if (!handle) {
        ++m_impl->plays_refused;
        return {};
    }
    m_impl->voices_peak =
        std::max(m_impl->voices_peak, static_cast<std::uint32_t>(m_impl->voices.size()));
    return *handle;
}

bool AudioDevice::stop(VoiceHandle voice) {
    ATLAS_ASSERT_MAIN_THREAD();
    return m_impl && m_impl->voices.destroy(voice);
}

bool AudioDevice::set_volume(VoiceHandle voice, float volume) {
    ATLAS_ASSERT_MAIN_THREAD();
    if (!m_impl) {
        return false;
    }
    VoiceState* state = m_impl->voices.get(voice);
    if (state == nullptr) {
        return false;
    }
    state->volume = std::clamp(volume, 0.0F, 4.0F);
    return true;
}

void AudioDevice::stop_all() {
    ATLAS_ASSERT_MAIN_THREAD();
    if (m_impl) {
        m_impl->voices.clear();
    }
}

void AudioDevice::set_bus_volume(Bus bus, float volume) {
    if (!m_impl || bus == Bus::Count) {
        return;
    }
    m_impl->bus_volume[static_cast<std::size_t>(bus)] = std::clamp(volume, 0.0F, 4.0F);
}

float AudioDevice::bus_volume(Bus bus) const noexcept {
    if (!m_impl || bus == Bus::Count) {
        return 0.0F;
    }
    return m_impl->bus_volume[static_cast<std::size_t>(bus)];
}

void AudioDevice::update() {
    ATLAS_ASSERT_MAIN_THREAD();
    if (!m_impl) {
        return;
    }
    ATLAS_ZONE_NAMED("AudioDevice::update");

    Impl& impl = *m_impl;

    if (impl.stream == nullptr) {
        // No device, so no queue to measure. Advance by real time instead, which is what makes
        // a one-second sound last a second whether or not anything can hear it.
        const auto elapsed = impl.clock.tick();
        if (!impl.advanced_once) {
            // The first interval is measured from construction rather than from the previous
            // update, so it says nothing about a frame. Discard it.
            impl.advanced_once = true;
            return;
        }
        const auto elapsed_ns =
            static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed.count(), 0));

        // Capped for the same reason the real path caps its passes: after a debugger pause,
        // grinding through minutes of audio helps nobody.
        const std::size_t wanted =
            std::min(static_cast<std::size_t>((elapsed_ns * kMixSampleRate) / 1'000'000'000ULL),
                     ms_to_frames(impl.config.queue_max_ms));
        std::size_t remaining = wanted;
        while (remaining > 0) {
            const std::size_t frames = std::min(remaining, kBlockFrames);
            impl.mix_into_block(frames);
            remaining -= frames;
        }
        return;
    }

    auto* stream = static_cast<SDL_AudioStream*>(impl.stream);
    const int queued_bytes = SDL_GetAudioStreamQueued(stream);
    const std::size_t bytes_per_frame = kMixChannels * sizeof(float);
    std::size_t queued_frames =
        queued_bytes > 0 ? static_cast<std::size_t>(queued_bytes) / bytes_per_frame : 0;

    if (queued_frames == 0 && !impl.voices.empty()) {
        // The queue ran dry with something still sounding, which is heard as a gap. Counted
        // every time; logged once, because a machine that underruns underruns every frame and
        // a log that says so is a log nobody reads.
        ++impl.underruns;
        if (!impl.warned_underrun) {
            impl.warned_underrun = true;
            ATLAS_LOG_WARN(kAudio,
                           "the audio queue ran dry with voices playing; a gap will have been "
                           "audible. Further underruns are counted but not logged");
        }
    }

    const std::size_t target_frames = ms_to_frames(impl.config.queue_target_ms);
    const std::size_t max_frames = ms_to_frames(impl.config.queue_max_ms);

    for (int pass = 0; pass < kMaxPassesPerUpdate && queued_frames < target_frames; ++pass) {
        const std::size_t room = max_frames > queued_frames ? max_frames - queued_frames : 0;
        const std::size_t frames = std::min(kBlockFrames, room);
        if (frames == 0) {
            break;
        }

        impl.mix_into_block(frames);

        if (!SDL_PutAudioStreamData(stream, impl.block.data(),
                                    static_cast<int>(frames * bytes_per_frame))) {
            // The stream refused the data. Nothing useful can be done about it here and the
            // next frame will try again; the silence is already counted as an underrun.
            break;
        }
        queued_frames += frames;
    }
}

AudioStats AudioDevice::stats() const noexcept {
    if (!m_impl) {
        return AudioStats{.null_device = true};
    }
    const Impl& impl = *m_impl;

    std::uint32_t queued_ms = 0;
    if (impl.stream != nullptr) {
        const int queued_bytes =
            SDL_GetAudioStreamQueued(static_cast<SDL_AudioStream*>(impl.stream));
        if (queued_bytes > 0) {
            queued_ms = frames_to_ms(static_cast<std::size_t>(queued_bytes) /
                                     (kMixChannels * sizeof(float)));
        }
    }

    return AudioStats{
        .voices = static_cast<std::uint32_t>(impl.voices.size()),
        .voices_peak = impl.voices_peak,
        .clips = static_cast<std::uint32_t>(impl.clips.size()),
        .queued_ms = queued_ms,
        .underruns = impl.underruns,
        .plays_refused = impl.plays_refused,
        .null_device = impl.stream == nullptr,
    };
}

}  // namespace atlas::audio

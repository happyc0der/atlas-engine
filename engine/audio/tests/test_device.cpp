// SPDX-License-Identifier: GPL-3.0-or-later
// The device, on the "dummy" audio driver: a real stream and a real queue with nothing behind
// them. That is what lets the push loop, the voice pool and the statistics be exercised on a
// runner with no sound card, following the arrangement test_window_dummy.cpp already uses for
// windows. What it cannot cover is anything a real driver decides: the actual device buffer
// size, a device disappearing, or whether any of this is audible.
#include <atlas/audio/device.hpp>
#include <atlas/audio/synth.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/platform/platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using atlas::audio::AudioDevice;
using atlas::audio::Bus;
using atlas::audio::kMixSampleRate;
using atlas::audio::PlayParams;
using atlas::audio::sine_blip;
using atlas::platform::Platform;

namespace {

/// The device asserts main-thread affinity, and `is_main_thread` answers false until some
/// thread has claimed it — deliberately, so an affinity check cannot pass by accident in a
/// process that never established a main thread. A test binary that builds a device without
/// a platform has to claim it itself. Same pattern as engine/assets/tests/test_registry.cpp.
const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// A platform with the dummy audio driver and no video, or nothing if it will not start.
[[nodiscard]] std::optional<Platform> dummy_audio_platform() {
    auto platform = Platform::create({.video = false, .audio = true, .audio_driver = "dummy"});
    if (!platform || !platform->has_audio_support()) {
        return std::nullopt;
    }
    return std::move(*platform);
}

/// A one-second mono clip, long enough that it does not finish inside a test's few updates.
[[nodiscard]] std::vector<float> long_tone() {
    return sine_blip(440.0F, 1000);
}

}  // namespace

TEST_CASE("a device opens on the dummy driver and closes cleanly", "[audio][device]") {
    const auto platform = dummy_audio_platform();
    if (!platform) {
        SKIP("the dummy audio driver is unavailable on this system");
    }

    auto device = AudioDevice::create();
    if (!device) {
        SKIP("no audio device could be opened even on the dummy driver");
    }
    CHECK_FALSE(device->is_null());
    CHECK(device->stats().null_device == false);
}

TEST_CASE("creating a device without the subsystem fails rather than pretending",
          "[audio][device]") {
    // No platform at all, so no audio subsystem. The point is that this is an error and not a
    // silent fallback: an application that cannot make a sound has to say so in its own log,
    // or the first symptom is somebody reporting that the game is quiet.
    auto device = AudioDevice::create();
    if (device) {
        // Another test in this binary may have left the subsystem up. Then this case has
        // nothing to say, which is honest; it is not evidence that the check works.
        SKIP("the audio subsystem is already running in this process");
    }
    CHECK(device.error().code() == atlas::ErrorCode::AudioInitFailed);
    CHECK(atlas::error_domain(device.error().code()) == atlas::ErrorDomain::Audio);
}

TEST_CASE("the null device accepts everything and reports itself", "[audio][device]") {
    auto device = AudioDevice::null();
    CHECK(device.is_null());
    CHECK(device.stats().null_device);

    const auto tone = long_tone();
    auto clip = device.create_clip({.samples = tone, .debug_name = "tone"});
    REQUIRE(clip.has_value());

    const auto voice = device.play(*clip);
    CHECK(voice.valid());
    CHECK(device.stats().voices == 1);

    // Advancing is by the clock, and the first update only establishes where the clock was,
    // so nothing is consumed by it. A second update advances by a frame's worth at most.
    device.update();
    device.update();
    CHECK(device.stats().voices == 1);
    CHECK(device.stats().underruns == 0);
}

TEST_CASE("a clip refuses audio the engine cannot represent", "[audio][device]") {
    auto device = AudioDevice::null();
    const std::vector<float> samples{0.0F, 0.1F, 0.2F, 0.3F};

    const auto no_channels = device.create_clip({.samples = samples, .channels = 0});
    REQUIRE_FALSE(no_channels.has_value());
    CHECK(no_channels.error().code() == atlas::ErrorCode::AudioFormatUnsupported);

    const auto surround = device.create_clip({.samples = samples, .channels = 6});
    REQUIRE_FALSE(surround.has_value());
    CHECK(surround.error().code() == atlas::ErrorCode::AudioFormatUnsupported);

    const auto impossible_rate = device.create_clip({.samples = samples, .sample_rate = 3});
    REQUIRE_FALSE(impossible_rate.has_value());
    CHECK(impossible_rate.error().code() == atlas::ErrorCode::AudioFormatUnsupported);

    // Three samples cannot be a whole number of stereo frames. Accepting it would read one
    // sample past the end of the last frame.
    const std::vector<float> ragged{0.0F, 0.1F, 0.2F};
    const auto half_frame = device.create_clip({.samples = ragged, .channels = 2});
    REQUIRE_FALSE(half_frame.has_value());
    CHECK(half_frame.error().code() == atlas::ErrorCode::InvalidArgument);
}

TEST_CASE("a clip is resampled to the mix rate when it is created", "[audio][device]") {
    auto device = AudioDevice::null();
    const std::vector<float> samples(1000, 0.5F);

    const auto clip = device.create_clip({.samples = samples, .sample_rate = kMixSampleRate / 2});
    REQUIRE(clip.has_value());
    // Nothing observable here says how long the clip is, which is the point of a handle. What
    // this pins is that a non-mix rate is accepted at all rather than refused or stored as-is.
    CHECK(device.stats().clips == 1);
}

TEST_CASE("a stale handle is refused by generation", "[audio][device]") {
    auto device = AudioDevice::null();
    const auto tone = long_tone();
    auto clip = device.create_clip({.samples = tone});
    REQUIRE(clip.has_value());

    const auto voice = device.play(*clip);
    REQUIRE(voice.valid());
    CHECK(device.stop(voice));

    // The same handle a second time resolves to nothing rather than to whatever now occupies
    // the slot. A voice handle outliving its voice is the normal case, not an error.
    CHECK_FALSE(device.stop(voice));
    CHECK_FALSE(device.set_volume(voice, 0.5F));

    // And a slot reused by a new voice does not answer to the old handle either.
    const auto replacement = device.play(*clip);
    REQUIRE(replacement.valid());
    CHECK_FALSE(device.stop(voice));
    CHECK(device.stop(replacement));
}

TEST_CASE("playing an unknown clip returns a null handle", "[audio][device]") {
    auto device = AudioDevice::null();
    const auto tone = long_tone();
    auto clip = device.create_clip({.samples = tone});
    REQUIRE(clip.has_value());
    CHECK(device.destroy_clip(*clip));

    const auto voice = device.play(*clip);
    CHECK_FALSE(voice.valid());
}

TEST_CASE("the voice limit refuses and counts rather than growing", "[audio][device]") {
    auto device = AudioDevice::null({.max_voices = 4});
    const auto tone = long_tone();
    auto clip = device.create_clip({.samples = tone});
    REQUIRE(clip.has_value());

    for (int i = 0; i < 4; ++i) {
        CHECK(device.play(*clip).valid());
    }
    // The fifth is refused with a null handle, not an error: dropping a sound under load is a
    // normal outcome, and a Result here would mean every call site ignored it.
    CHECK_FALSE(device.play(*clip).valid());

    const auto stats = device.stats();
    CHECK(stats.voices == 4);
    CHECK(stats.voices_peak == 4);
    CHECK(stats.plays_refused == 1);
}

TEST_CASE("voices still sounding finish on the buffer they started with", "[audio][device]") {
    auto device = AudioDevice::null();
    const auto tone = long_tone();
    auto clip = device.create_clip({.samples = tone});
    REQUIRE(clip.has_value());

    const auto voice = device.play(*clip);
    REQUIRE(voice.valid());

    // Releasing the clip while it is sounding must not pull the buffer out from under the
    // voice. This is the case a hot reload hits every time, and the reason a clip is held by
    // shared_ptr rather than by index.
    CHECK(device.destroy_clip(*clip));
    device.update();
    device.update();
    CHECK(device.stats().voices == 1);
    CHECK(device.stats().clips == 0);
}

TEST_CASE("stop_all retires every voice", "[audio][device]") {
    auto device = AudioDevice::null();
    const auto tone = long_tone();
    auto clip = device.create_clip({.samples = tone});
    REQUIRE(clip.has_value());

    for (int i = 0; i < 3; ++i) {
        CHECK(device.play(*clip).valid());
    }
    device.stop_all();
    CHECK(device.stats().voices == 0);
    // The peak is a high-water mark and does not reset, which is what makes it useful in an
    // exit summary.
    CHECK(device.stats().voices_peak == 3);
}

TEST_CASE("bus volumes are clamped and readable", "[audio][device]") {
    auto device = AudioDevice::null();
    CHECK(device.bus_volume(Bus::Master) == 1.0F);

    device.set_bus_volume(Bus::Music, 0.25F);
    CHECK(device.bus_volume(Bus::Music) == 0.25F);

    device.set_bus_volume(Bus::Effects, -1.0F);
    CHECK(device.bus_volume(Bus::Effects) == 0.0F);

    // Above one is a boost, which is useful; unbounded is not.
    device.set_bus_volume(Bus::Effects, 100.0F);
    CHECK(device.bus_volume(Bus::Effects) == 4.0F);
}

TEST_CASE("repeated updates keep the queue in band with no underruns", "[audio][device]") {
    const auto platform = dummy_audio_platform();
    if (!platform) {
        SKIP("the dummy audio driver is unavailable on this system");
    }
    auto device = AudioDevice::create({.queue_target_ms = 60, .queue_max_ms = 120});
    if (!device) {
        SKIP("no audio device could be opened even on the dummy driver");
    }

    const auto tone = long_tone();
    auto clip = device->create_clip({.samples = tone, .debug_name = "tone"});
    REQUIRE(clip.has_value());
    REQUIRE(device->play(*clip, PlayParams{.loop = true}).valid());

    // Two hundred updates with no wait between them, which is a far harder case than a real
    // frame loop: nothing drains the queue, so the push loop's own ceiling is the only thing
    // stopping it from queueing without bound.
    for (int i = 0; i < 200; ++i) {
        device->update();
    }

    const auto stats = device->stats();
    CHECK(stats.queued_ms <= 120);
    CHECK(stats.voices == 1);
    // The first update finds an empty queue, which is not an underrun because it is the
    // starting condition rather than a gap. After that the queue must never run dry.
    CHECK(stats.underruns <= 1);
}

TEST_CASE("a moved-from device is inert rather than dangerous", "[audio][device]") {
    auto device = AudioDevice::null();
    const auto tone = long_tone();
    auto clip = device.create_clip({.samples = tone});
    REQUIRE(clip.has_value());
    REQUIRE(device.play(*clip).valid());

    AudioDevice moved = std::move(device);
    CHECK(moved.stats().voices == 1);

    // The moved-from object must answer every query rather than dereferencing nothing. Its
    // destructor runs at the end of this scope and must not close a stream it gave away.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK(device.stats().null_device);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK_FALSE(device.play(*clip).valid());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    device.update();
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Turning bytes on disk into something the engine can use.
///
/// An importer runs on a worker thread and must therefore touch nothing that belongs to the
/// main thread: no window, no graphics device, no engine-wide state. It takes bytes and
/// produces a description plus decoded data, and whoever owns the device turns that into a
/// resource later, on the thread that is allowed to.
///
/// That split is the whole reason loading can be asynchronous at all.

#include <atlas/assets/asset_id.hpp>
#include <atlas/assets/virtual_path.hpp>
#include <atlas/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace atlas::assets {

/// A decoded image, ready to become a texture.
struct ImportedTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Four channels, eight bits each, in red-green-blue-alpha order, tightly packed. One
    /// layout rather than several, because every format the engine draws with today is this
    /// one and converting at import is cheaper than branching at every use.
    std::vector<std::byte> pixels;
};

/// A compiled shader, ready to become a graphics resource.
struct ImportedShader {
    std::vector<std::byte> code;
    std::string entry_point;
    bool is_vertex_stage = true;
    std::uint32_t samplers = 0;
    std::uint32_t storage_textures = 0;
    std::uint32_t storage_buffers = 0;
    std::uint32_t uniform_buffers = 0;
};

/// Decoded audio, ready to become a clip.
///
/// Float samples in the file's own rate and channel count, interleaved. Converting to the
/// engine's mix rate needs a clip pool and belongs to whoever owns one; an importer runs on a
/// worker and produces bytes, nothing more.
struct ImportedAudio {
    std::vector<float> samples;
    std::uint32_t channels = 1;
    std::uint32_t sample_rate = 0;
};

/// One key on a clip's transform track, as it came out of a file.
///
/// Plain numbers, in nanoseconds. Deliberately not the animation module's own key type: an
/// importer runs on a worker inside this module, which cannot see that one, and a payload that
/// needed it would put file decoding and clip evaluation in the same place.
struct ImportedTransformKey {
    std::uint64_t time_ns = 0;
    float position_x = 0.0F;
    float position_y = 0.0F;
    float rotation = 0.0F;
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    /// The easing's index in the animation module's own enumeration. The two agree by name in
    /// the file and by position here; the parser refuses a name it does not know rather than
    /// choosing one.
    std::uint8_t easing = 0;
};

/// One key on a clip's frame track.
struct ImportedFrameKey {
    std::uint64_t time_ns = 0;
    std::uint32_t cell = 0;
};

/// A decoded animation clip, ready to become one the module can sample.
struct ImportedAnimationClip {
    std::string name;
    std::uint64_t duration_ns = 0;
    std::uint32_t columns = 1;
    std::uint32_t rows = 1;
    std::vector<ImportedTransformKey> transform_keys;
    std::vector<ImportedFrameKey> frame_keys;
};

/// Decode an image.
///
/// Thread-safe and free of engine state, so it can run on a worker.
[[nodiscard]] Result<ImportedTexture> import_texture(std::span<const std::byte> bytes,
                                                     std::string_view debug_name);

/// Decode audio.
///
/// **The format is decided by the leading bytes, never by the path's extension.** An extension
/// is a claim made by whoever named the file, and this function's input is untrusted; the
/// magic bytes are the only part of that claim the file has to honour to be decodable at all.
///
/// Today that means RIFF/WAVE and nothing else. Anything unrecognised is
/// `AssetImportFailed` naming what was found, rather than a guess.
///
/// Thread-safe and free of engine state, so it can run on a worker.
[[nodiscard]] Result<ImportedAudio> import_audio(std::span<const std::byte> bytes,
                                                 std::string_view debug_name);

/// Decode an animation clip.
///
/// The format names and versions itself, and everything it claims is checked: the document's
/// own length before it is parsed at all, then every count, every ordering, and every value.
///
/// Thread-safe and free of engine state, so it can run on a worker.
[[nodiscard]] Result<ImportedAnimationClip> import_animation_clip(std::span<const std::byte> bytes,
                                                                  std::string_view debug_name);

/// Limits on what will be decoded.
///
/// An image header is untrusted input, and a header claiming enormous dimensions is the
/// standard way to turn a decode into an allocation failure or worse. These bounds are
/// generous for anything real and fatal to anything absurd.
struct ImportLimits {
    std::uint32_t max_texture_dimension = 16384;
    /// Largest decoded image, in bytes. Sixteen thousand squared at four bytes a pixel is
    /// about a gigabyte, so this is the binding limit in practice.
    std::uint64_t max_texture_bytes = 256ULL * 1024 * 1024;

    /// Largest decoded clip, in bytes of float samples. Sixty-four megabytes is about six
    /// minutes of stereo at the mix rate: generous for anything held in memory in full, and
    /// fatal to a header claiming four billion frames.
    std::uint64_t max_audio_bytes = 64ULL * 1024 * 1024;
    /// Rates outside this are not a format this engine has not heard of; they are a file
    /// lying about itself.
    std::uint32_t min_audio_sample_rate = 8'000;
    std::uint32_t max_audio_sample_rate = 192'000;
    /// Mono and stereo. More would mean deciding how to fold them down, and nothing asks.
    std::uint32_t max_audio_channels = 2;

    /// The largest clip document, in bytes, **checked before it is parsed**.
    ///
    /// This is the one bound a document parser genuinely needs, and it is not the same
    /// discipline the audio reader uses. That reader compares a declared size against the
    /// bytes actually present before allocating anything. A parser cannot: by the time any
    /// count inside the document can be read, the whole document is already in memory. So the
    /// only place to refuse an enormous one is before the parse begins.
    ///
    /// A megabyte is enormous for a clip. The committed ones are a few hundred bytes.
    std::uint64_t max_clip_bytes = 1024ULL * 1024;
    /// Keys on either track. A clip is authored by a person or exported by a tool; either way
    /// a hundred thousand of them is a mistake or an attack.
    std::uint32_t max_clip_keys = 100'000;
    /// Cells a sheet may be divided into, on each axis.
    std::uint32_t max_clip_grid = 4096;
    /// A clip longer than a day is not a clip.
    std::uint64_t max_clip_duration_ms = 86'400'000;
};

[[nodiscard]] const ImportLimits& import_limits() noexcept;

}  // namespace atlas::assets

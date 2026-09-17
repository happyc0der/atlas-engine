// SPDX-License-Identifier: GPL-3.0-or-later
//
// A RIFF/WAVE reader, written here rather than borrowed.
//
// The obvious alternative is the window system's own loader, and it cannot be used: importing
// happens inside `assets`, which links no SDL and must not, so reaching for it would mean
// moving the importer into a module that does or widening a boundary for one function. The
// second alternative is the simulation's `SaveReader`, which already has exactly the right
// bounds discipline — and lives in `atlas::sim`, which `assets` does not and must not depend
// on. So the discipline is reproduced here, following `artifact_cache.cpp`'s shape, which is
// the one hardened reader that already lives in this module.
//
// **Every byte of this is untrusted input.** A length in a file is a claim, and the standard
// attack is a four-byte field claiming four billion frames so that the reader allocates before
// it checks. Nothing here allocates on the strength of a declared size: every size is compared
// against the bytes that are actually present first.

#include "wav.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>

namespace atlas::assets::detail {
namespace {

/// Format tags, from the specification. Anything else is refused by number.
constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

/// A chunk header is an identifier and a size. Everything in the file is one of these.
constexpr std::size_t kChunkHeaderBytes = 8;
/// The smallest `fmt ` chunk the specification allows.
constexpr std::size_t kMinFormatChunkBytes = 16;
/// "RIFF" plus a size plus "WAVE".
constexpr std::size_t kRiffHeaderBytes = 12;

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
    // Little-endian by hand rather than by memcpy plus a byte swap, so the code reads the same
    // on either endianness and needs no conditional compilation to be correct on both.
    // Widened to unsigned before shifting. A uint16_t promotes to `int`, which is signed, so
    // shifting it is a signed bitwise operation even though every value involved is positive.
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(bytes[offset]) |
                                      (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool tag_is(std::span<const std::byte> bytes, std::size_t offset,
                          std::string_view tag) noexcept {
    if (offset + tag.size() > bytes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < tag.size(); ++i) {
        if (static_cast<char>(bytes[offset + i]) != tag[i]) {
            return false;
        }
    }
    return true;
}

/// One sample of `bits` bits at `offset`, normalised to [-1, 1].
[[nodiscard]] float decode_sample(std::span<const std::byte> bytes, std::size_t offset,
                                  std::uint16_t bits, bool is_float) noexcept {
    if (is_float) {
        // Bit-cast rather than reinterpret: the bytes are little-endian in the file and this
        // is the only place the reader assumes the host agrees, which every platform Atlas
        // targets does.
        return std::bit_cast<float>(read_u32(bytes, offset));
    }
    switch (bits) {
    case 8:
        // Eight-bit PCM is unsigned with 128 as silence, alone among the widths. Treating it
        // as signed like the others produces a loud square wave, which is the single most
        // recognisable symptom of getting this wrong.
        return (static_cast<float>(static_cast<std::uint8_t>(bytes[offset])) - 128.0F) / 128.0F;
    case 16: {
        const auto raw = static_cast<std::int16_t>(read_u16(bytes, offset));
        return static_cast<float>(raw) / 32768.0F;
    }
    case 24: {
        const std::uint32_t raw = static_cast<std::uint32_t>(bytes[offset]) |
                                  (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
                                  (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U);
        // Sign-extend from 24 bits, then scale.
        const auto signed_raw =
            static_cast<std::int32_t>((raw & 0x0080'0000U) != 0 ? raw | 0xFF00'0000U : raw);
        return static_cast<float>(signed_raw) / 8388608.0F;
    }
    case 32: {
        const auto raw = static_cast<std::int32_t>(read_u32(bytes, offset));
        return static_cast<float>(raw) / 2147483648.0F;
    }
    default: return 0.0F;  // Refused before reaching here.
    }
}

}  // namespace

bool looks_like_wav(std::span<const std::byte> bytes) noexcept {
    return bytes.size() >= kRiffHeaderBytes && tag_is(bytes, 0, "RIFF") && tag_is(bytes, 8, "WAVE");
}

Result<ImportedAudio> import_wav(std::span<const std::byte> bytes, std::string_view debug_name) {
    const auto refuse = [&debug_name](std::string message) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("'{}': {}", debug_name, message)));
    };

    if (!looks_like_wav(bytes)) {
        return refuse("not a RIFF/WAVE file");
    }

    // The declared RIFF size covers everything after the first eight bytes. A file claiming
    // more than it has is truncated, and reading on would walk off the end of the buffer.
    const std::uint32_t declared = read_u32(bytes, 4);
    if (static_cast<std::uint64_t>(declared) + 8ULL > bytes.size()) {
        return refuse(std::format("claims {} bytes but holds {}", declared + 8U, bytes.size()));
    }

    const ImportLimits& limits = import_limits();

    std::uint16_t format_tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits = 0;
    bool have_format = false;
    std::size_t data_offset = 0;
    std::size_t data_bytes = 0;
    bool have_data = false;

    // Walk the chunks. Unknown ones are skipped, which is the whole reason a WAVE file can
    // carry titles and loop points without every reader having to understand them.
    std::size_t offset = kRiffHeaderBytes;
    while (offset + kChunkHeaderBytes <= bytes.size()) {
        const std::uint32_t chunk_size = read_u32(bytes, offset + 4);
        const std::size_t body = offset + kChunkHeaderBytes;
        if (static_cast<std::uint64_t>(body) + chunk_size > bytes.size()) {
            return refuse(std::format("a chunk at offset {} claims {} bytes, past the end of the "
                                      "file",
                                      offset, chunk_size));
        }

        if (tag_is(bytes, offset, "fmt ")) {
            if (chunk_size < kMinFormatChunkBytes) {
                return refuse(std::format("the format chunk is {} bytes, too small to describe "
                                          "anything",
                                          chunk_size));
            }
            format_tag = read_u16(bytes, body);
            channels = read_u16(bytes, body + 2);
            sample_rate = read_u32(bytes, body + 4);
            bits = read_u16(bytes, body + 14);
            if (format_tag == kFormatExtensible) {
                // The real tag lives in the extension's own header. Without it, an extensible
                // float file would be decoded as integer and come out as noise.
                if (chunk_size < 40) {
                    return refuse("an extensible format chunk is too small to hold its own tag");
                }
                format_tag = read_u16(bytes, body + 24);
            }
            have_format = true;
        } else if (tag_is(bytes, offset, "data")) {
            data_offset = body;
            data_bytes = chunk_size;
            have_data = true;
        }

        // Chunks are padded to an even length, and the pad byte is not counted in the size.
        // Missing this walks every subsequent chunk one byte out of alignment, which reads as
        // a corrupt file rather than as an off-by-one.
        const std::size_t advance = kChunkHeaderBytes + chunk_size + (chunk_size % 2);
        if (advance == 0) {
            return refuse("a chunk of no length, which cannot be advanced past");
        }
        offset += advance;
    }

    if (!have_format) {
        return refuse("has no format chunk");
    }
    if (!have_data) {
        return refuse("has no data chunk");
    }

    if (format_tag != kFormatPcm && format_tag != kFormatFloat) {
        return refuse(
            std::format("uses format {:#06x}, which is neither integer nor float PCM", format_tag));
    }
    const bool is_float = format_tag == kFormatFloat;
    if (is_float && bits != 32) {
        return refuse(std::format("is float PCM at {} bits; only 32 is defined", bits));
    }
    if (!is_float && bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        return refuse(
            std::format("is {}-bit integer PCM, which this reader does not decode", bits));
    }
    if (channels == 0 || channels > limits.max_audio_channels) {
        return refuse(
            std::format("has {} channels; the engine decodes mono and stereo only", channels));
    }
    if (sample_rate < limits.min_audio_sample_rate || sample_rate > limits.max_audio_sample_rate) {
        return refuse(std::format("claims {} Hz, outside [{}, {}]", sample_rate,
                                  limits.min_audio_sample_rate, limits.max_audio_sample_rate));
    }

    const std::size_t bytes_per_sample = bits / 8U;
    const std::size_t bytes_per_frame = bytes_per_sample * channels;
    if (bytes_per_frame == 0) {
        return refuse("describes frames of no length");
    }
    // Truncate rather than refuse a data chunk that is not a whole number of frames. A file
    // cut short mid-frame is a damaged file, and playing all of it but the last fragment is a
    // better answer than refusing a sound that is otherwise entirely intact.
    const std::size_t frames = data_bytes / bytes_per_frame;
    if (frames == 0) {
        return refuse("has no audio frames");
    }

    // The check that actually stops a cheap attack, and it happens **before** the allocation
    // below rather than after it.
    const std::uint64_t decoded_bytes =
        static_cast<std::uint64_t>(frames) * channels * sizeof(float);
    if (decoded_bytes > limits.max_audio_bytes) {
        return refuse(std::format("would decode to {} bytes, past the limit of {}", decoded_bytes,
                                  limits.max_audio_bytes));
    }

    ImportedAudio audio;
    audio.channels = channels;
    audio.sample_rate = sample_rate;
    audio.samples.resize(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            const std::size_t at = data_offset + (frame * bytes_per_frame) +
                                   (static_cast<std::size_t>(channel) * bytes_per_sample);
            audio.samples[(frame * channels) + channel] = decode_sample(bytes, at, bits, is_float);
        }
    }
    return audio;
}

}  // namespace atlas::assets::detail

// SPDX-License-Identifier: GPL-3.0-or-later
// The RIFF/WAVE reader, driven directly, with every input built here.
//
// Nothing on disk. Following the image tests, which embed a seventy-five byte PNG rather than
// committing a fixture: an input assembled by the test is an input the test can make hostile
// on purpose, one field at a time, which is the only way to check a reader that exists to
// survive hostile input.
//
// The cases below are the ones that turn a decode into an allocation failure or a read past
// the end of a buffer. They run under the address sanitizer in CI, where a read past the end
// is a failure rather than a value nobody notices.
#include <atlas/assets/importer.hpp>

#include "../src/wav.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using atlas::assets::import_audio;
using atlas::assets::detail::import_wav;
using atlas::assets::detail::looks_like_wav;
using Catch::Approx;

namespace {

void put_u16(std::string& out, std::uint16_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
}

void put_u32(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
    out += static_cast<char>((value >> 16U) & 0xFFU);
    out += static_cast<char>((value >> 24U) & 0xFFU);
}

struct WavParts {
    std::uint16_t format_tag = 1;
    std::uint16_t channels = 1;
    std::uint32_t sample_rate = 48'000;
    std::uint16_t bits = 16;
    std::string data;
    /// Written into the data chunk's size field instead of the real length, when set. This is
    /// the field a hostile file lies about.
    std::optional<std::uint32_t> declared_data_size;
    /// Written into the RIFF size field instead of the real length, when set.
    std::optional<std::uint32_t> declared_riff_size;
    /// An extra chunk before the data, to exercise skipping. Even length, so it needs no pad.
    bool include_info_chunk = false;
    /// The same, but an odd length, so the reader has to step over the pad byte too.
    bool odd_info_chunk = false;
    /// Write an extensible format chunk, whose real format tag lives in its extension.
    bool extensible = false;
    /// The sub-format tag inside an extensible chunk.
    std::uint16_t sub_format_tag = 3;
};

[[nodiscard]] std::string build(const WavParts& parts) {
    std::string fmt;
    put_u16(fmt, parts.extensible ? std::uint16_t{0xFFFE} : parts.format_tag);
    put_u16(fmt, parts.channels);
    put_u32(fmt, parts.sample_rate);
    put_u32(fmt, parts.sample_rate * parts.channels * (parts.bits / 8U));
    put_u16(fmt, static_cast<std::uint16_t>(parts.channels * (parts.bits / 8U)));
    put_u16(fmt, parts.bits);

    if (parts.extensible) {
        // The extension, whose last field is a sixteen-byte identifier beginning with the
        // format tag that actually applies. A reader that stops at the first tag sees only
        // 0xFFFE, which says "look in here" and nothing else.
        put_u16(fmt, 22);          // extension size
        put_u16(fmt, parts.bits);  // valid bits per sample
        put_u32(fmt, 0);           // channel mask
        put_u16(fmt, parts.sub_format_tag);
        fmt.append(14, '\0');  // the rest of the identifier
    }

    std::string chunks;
    chunks += "fmt ";
    put_u32(chunks, static_cast<std::uint32_t>(fmt.size()));
    chunks += fmt;

    if (parts.include_info_chunk) {
        chunks += "LIST";
        put_u32(chunks, 18);
        chunks.append("INFO", 4);
        chunks.append("ICMT", 4);
        put_u32(chunks, 6);
        chunks.append("hello", 5);
        chunks += '\0';
    }

    if (parts.odd_info_chunk) {
        // Five bytes of body, so the chunk's total length is odd and a pad byte follows it
        // without being counted in the size. That pad is what a reader forgets to step over.
        chunks += "note";
        put_u32(chunks, 5);
        chunks.append("odd!!", 5);
        chunks += '\0';
    }

    chunks += "data";
    put_u32(chunks,
            parts.declared_data_size.value_or(static_cast<std::uint32_t>(parts.data.size())));
    chunks += parts.data;

    std::string file = "RIFF";
    put_u32(file, parts.declared_riff_size.value_or(static_cast<std::uint32_t>(4 + chunks.size())));
    file += "WAVE";
    file += chunks;
    return file;
}

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string& text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

/// Thirty-two-bit floats, little-endian, as the data chunk's payload.
[[nodiscard]] std::string float32(const std::vector<float>& values) {
    std::string out;
    for (const auto value : values) {
        put_u32(out, std::bit_cast<std::uint32_t>(value));
    }
    return out;
}

/// Sixteen-bit samples, little-endian, as the data chunk's payload.
[[nodiscard]] std::string pcm16(const std::vector<std::int16_t>& values) {
    std::string out;
    for (const auto value : values) {
        put_u16(out, static_cast<std::uint16_t>(value));
    }
    return out;
}

}  // namespace

TEST_CASE("a well-formed file decodes to the samples it holds", "[assets][wav]") {
    WavParts parts;
    parts.data = pcm16({0, 16384, -16384, 32767});

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "tone.wav");
    REQUIRE(audio.has_value());

    CHECK(audio->channels == 1);
    CHECK(audio->sample_rate == 48'000);
    REQUIRE(audio->samples.size() == 4);
    // Checked by value, not only by count: a reader with the wrong stride or the wrong sign
    // convention produces exactly the right number of samples and the wrong sound.
    CHECK(audio->samples[0] == Approx(0.0F).margin(1e-6));
    CHECK(audio->samples[1] == Approx(0.5F).margin(1e-4));
    CHECK(audio->samples[2] == Approx(-0.5F).margin(1e-4));
    CHECK(audio->samples[3] == Approx(1.0F).margin(1e-4));
}

TEST_CASE("eight-bit audio is unsigned, unlike every other width", "[assets][wav]") {
    WavParts parts;
    parts.bits = 8;
    // 128 is silence at eight bits. Decoded as signed it would be near full scale, which is
    // the loudest possible way to get this wrong.
    parts.data = std::string{"\x80\xFF\x00", 3};

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "eight.wav");
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() == 3);
    CHECK(audio->samples[0] == Approx(0.0F).margin(1e-6));
    CHECK(audio->samples[1] == Approx(1.0F).margin(0.01));
    CHECK(audio->samples[2] == Approx(-1.0F).margin(1e-6));
}

TEST_CASE("stereo samples stay interleaved and in order", "[assets][wav]") {
    WavParts parts;
    parts.channels = 2;
    parts.data = pcm16({32767, 0, 0, 32767});

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "stereo.wav");
    REQUIRE(audio.has_value());
    CHECK(audio->channels == 2);
    REQUIRE(audio->samples.size() == 4);
    CHECK(audio->samples[0] == Approx(1.0F).margin(1e-4));
    CHECK(audio->samples[1] == Approx(0.0F).margin(1e-6));
    CHECK(audio->samples[2] == Approx(0.0F).margin(1e-6));
    CHECK(audio->samples[3] == Approx(1.0F).margin(1e-4));
}

TEST_CASE("an unknown chunk before the data is skipped", "[assets][wav]") {
    // A reader that does not skip correctly walks every later chunk out of alignment and
    // reports a corrupt file, which looks like a broken asset rather than a broken reader.
    WavParts parts;
    parts.include_info_chunk = true;
    parts.data = pcm16({32767, -32768});

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "titled.wav");
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() == 2);
    CHECK(audio->samples[0] == Approx(1.0F).margin(1e-4));
    CHECK(audio->samples[1] == Approx(-1.0F).margin(1e-6));
}

TEST_CASE("the committed assets decode", "[assets][wav]") {
    // The generator and the reader agree. Neither working alone implies it: the script could
    // write a chunk layout this reader mishandles, and the reader could accept everything
    // except what the script produces. So these are the real bytes, read from disk.
    //
    // Skipped rather than failed when the file is not found, because running the test binary
    // by hand from somewhere other than the repository root is a normal thing to do and is
    // not a defect in the reader. Under ctest the working directory is the repository root,
    // so the skip does not fire where it matters.
    struct Expectation {
        const char* path;
        std::uint32_t sample_rate;
        std::uint32_t channels;
    };

    const std::array<Expectation, 2> expected{{
        {"assets/source/audio/click.wav", 48'000, 1},
        {"assets/source/audio/ambient_loop.wav", 22'050, 1},
    }};

    for (const auto& item : expected) {
        std::ifstream file{item.path, std::ios::binary};
        if (!file) {
            SKIP("the committed audio assets are not reachable from this working directory");
        }
        const std::string contents{std::istreambuf_iterator<char>{file},
                                   std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(contents.empty());

        const auto audio = import_wav(bytes_of(contents), item.path);
        REQUIRE(audio.has_value());
        CHECK(audio->sample_rate == item.sample_rate);
        CHECK(audio->channels == item.channels);
        CHECK_FALSE(audio->samples.empty());

        // Nothing clipped, and not silent either. A generator bug that wrote zeros would
        // otherwise pass every structural check above.
        const auto [low, high] = std::ranges::minmax_element(audio->samples);
        CHECK(*low >= -1.0F);
        CHECK(*high <= 1.0F);
        CHECK((*high - *low) > 0.1F);
    }
}

// ---------------------------------------------------------------- hostile input from here on

TEST_CASE("a data chunk claiming more than the file holds is refused", "[assets][wav]") {
    // The cheapest attack there is: a four-byte field claiming four billion frames, so that a
    // reader allocates before it checks. Nothing is allocated on the strength of this number.
    WavParts parts;
    parts.data = pcm16({0, 0});
    parts.declared_data_size = 0xFFFF'FFF0U;

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "greedy.wav");
    REQUIRE_FALSE(audio.has_value());
    CHECK(audio.error().code() == atlas::ErrorCode::MalformedData);
}

TEST_CASE("an extensible chunk is decoded by its real format, not by 0xFFFE", "[assets][wav]") {
    // An extensible file says only "the tag is inside the extension" in its first field. A
    // reader that stops there and assumes integer decodes a float file as fixed point, which
    // produces plausible-looking values at roughly half the right amplitude rather than
    // silence or a crash — the kind of wrong that is heard and not seen.
    WavParts parts;
    parts.extensible = true;
    parts.sub_format_tag = 3;  // float
    parts.bits = 32;
    parts.data = float32({1.0F, -1.0F, 0.5F});

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "extensible.wav");
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() == 3);
    CHECK(audio->samples[0] == Approx(1.0F).margin(1e-6));
    CHECK(audio->samples[1] == Approx(-1.0F).margin(1e-6));
    CHECK(audio->samples[2] == Approx(0.5F).margin(1e-6));
}

TEST_CASE("a chunk overrunning the file is refused before it is read", "[assets][wav]") {
    // The case the greedy-size test above does **not** cover, which mutation testing found by
    // removing the chunk-bounds check and watching every test still pass. A data chunk
    // claiming four billion bytes is stopped by the decoded-size limit even with the bounds
    // check gone; a chunk claiming ten thousand bytes in a file that holds a hundred is not,
    // because ten thousand bytes of audio is a perfectly reasonable amount to decode.
    //
    // Without the bounds check this reads roughly ten thousand bytes past the end of the
    // buffer. That is the actual memory-safety hole here, and this is the only case that
    // reaches it. It matters most under the address sanitizer, where the read is a failure
    // rather than whatever happened to be in memory.
    WavParts parts;
    parts.data = pcm16({1, 2, 3, 4});
    parts.declared_data_size = 10'000;
    // The RIFF size is made honest about the file's real length, so that check cannot be the
    // one that fires and mask the one being tested.
    const auto honest = build(parts);
    parts.declared_riff_size = static_cast<std::uint32_t>(honest.size() - 8);

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "overrun.wav");
    REQUIRE_FALSE(audio.has_value());
    CHECK(audio.error().code() == atlas::ErrorCode::MalformedData);
}

TEST_CASE("an odd-length chunk is skipped over its pad byte", "[assets][wav]") {
    // Chunks are padded to an even length and the pad byte is not counted in the size. A
    // reader that ignores this walks every later chunk one byte out of alignment, so the data
    // chunk is never found and an entirely valid file is reported as corrupt.
    //
    // The metadata chunk in the other cases happens to be an even length, so none of them
    // exercises the pad at all. This one is odd on purpose.
    WavParts parts;
    parts.odd_info_chunk = true;
    parts.data = pcm16({32767, -32768});

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "odd-chunk.wav");
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() == 2);
    CHECK(audio->samples[0] == Approx(1.0F).margin(1e-4));
}

TEST_CASE("audio too large to hold in memory is refused", "[assets][wav]") {
    // The resource cap, tested at the only scale that can reach it. Eight-bit mono expands
    // four times on decode, so seventeen megabytes of samples become sixty-eight, past the
    // sixty-four megabyte limit. Every smaller input is stopped by the bounds check instead,
    // which is why removing this limit survived every other case here.
    //
    // The allocation being refused is the point: the check happens before the resize, so this
    // never allocates the sixty-eight megabytes it is refusing.
    WavParts parts;
    parts.bits = 8;
    parts.data = std::string(17ULL * 1024 * 1024, '\x80');

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "enormous.wav");
    REQUIRE_FALSE(audio.has_value());
    CHECK(audio.error().code() == atlas::ErrorCode::MalformedData);
}

TEST_CASE("a RIFF size larger than the file is refused", "[assets][wav]") {
    WavParts parts;
    parts.data = pcm16({0, 0});
    parts.declared_riff_size = 0x7FFF'FFFFU;

    const auto file = build(parts);
    CHECK_FALSE(import_wav(bytes_of(file), "greedy-riff.wav").has_value());
}

TEST_CASE("a truncated file is refused rather than partly decoded", "[assets][wav]") {
    WavParts parts;
    parts.data = pcm16({1, 2, 3, 4, 5, 6, 7, 8});
    const auto whole = build(parts);

    // Cut at every length. Not one chosen truncation: the interesting cuts are the ones in
    // the middle of a length field, and picking them by hand means picking the ones already
    // thought of.
    for (std::size_t length = 0; length < whole.size(); ++length) {
        const std::string cut = whole.substr(0, length);
        const auto audio = import_wav(bytes_of(cut), "cut.wav");
        if (audio.has_value()) {
            // A cut that lands after a whole data chunk is a shorter but valid file, which is
            // fine. What must never happen is a decode that reports more frames than the
            // bytes could hold.
            CHECK(audio->samples.size() * sizeof(std::int16_t) <= cut.size());
        }
    }
}

TEST_CASE("garbage and emptiness are refused", "[assets][wav]") {
    CHECK_FALSE(looks_like_wav({}));
    CHECK_FALSE(import_wav({}, "empty.wav").has_value());

    const std::string garbage = "this is not audio in any format at all, not even close";
    CHECK_FALSE(looks_like_wav(bytes_of(garbage)));
    CHECK_FALSE(import_wav(bytes_of(garbage), "garbage.wav").has_value());

    // "RIFF" without "WAVE" is some other RIFF format, and being nearly right is exactly the
    // case a magic-byte check has to get right.
    std::string other = "RIFF";
    put_u32(other, 16);
    other += "AVI ";
    CHECK_FALSE(looks_like_wav(bytes_of(other)));
}

TEST_CASE("impossible formats are refused by number", "[assets][wav]") {
    const auto refused = [](const WavParts& parts) {
        const auto file = build(parts);
        return !import_wav(bytes_of(file), "odd.wav").has_value();
    };

    WavParts zero_channels;
    zero_channels.channels = 0;
    zero_channels.data = pcm16({0, 0});
    CHECK(refused(zero_channels));

    WavParts surround;
    surround.channels = 6;
    surround.data = pcm16({0, 0, 0, 0, 0, 0});
    CHECK(refused(surround));

    WavParts no_rate;
    no_rate.sample_rate = 0;
    no_rate.data = pcm16({0, 0});
    CHECK(refused(no_rate));

    WavParts absurd_rate;
    absurd_rate.sample_rate = 4'000'000;
    absurd_rate.data = pcm16({0, 0});
    CHECK(refused(absurd_rate));

    WavParts odd_width;
    odd_width.bits = 12;
    odd_width.data = pcm16({0, 0});
    CHECK(refused(odd_width));

    WavParts compressed;
    compressed.format_tag = 0x0011;  // IMA ADPCM: a real tag this reader does not decode
    compressed.data = pcm16({0, 0});
    CHECK(refused(compressed));
}

TEST_CASE("a file with no audio in it is refused", "[assets][wav]") {
    WavParts empty_data;
    empty_data.data = {};
    const auto file = build(empty_data);
    CHECK_FALSE(import_wav(bytes_of(file), "silent.wav").has_value());
}

TEST_CASE("a data chunk cut mid-frame keeps the whole frames it has", "[assets][wav]") {
    // A damaged file, not a hostile one. Playing all of it but the final fragment is a better
    // answer than refusing a sound that is otherwise entirely intact, and the decision is
    // pinned here so it does not drift into a refusal by accident.
    WavParts parts;
    parts.channels = 2;
    parts.data = pcm16({100, 200, 300});  // three samples: one whole stereo frame and a half

    const auto file = build(parts);
    const auto audio = import_wav(bytes_of(file), "ragged.wav");
    REQUIRE(audio.has_value());
    CHECK(audio->samples.size() == 2);
}

// ------------------------------------------------- the dispatcher, which chooses by content

TEST_CASE("audio is decoded by what it contains, not by what it is called", "[assets][wav]") {
    // The invariant this whole entry point exists for. An extension is a claim made by
    // whoever named the file, and the input here is untrusted, so the name carries no weight
    // at all: the same bytes decode under any name, and bytes that are not audio are refused
    // under every name including a convincing one.
    WavParts parts;
    parts.data = pcm16({32767, -32768});
    const auto wav = build(parts);

    for (const char* name : {"loop.wav", "loop.ogg", "loop.png", "loop", "loop.exe"}) {
        const auto audio = import_audio(bytes_of(wav), name);
        INFO("named " << name);
        REQUIRE(audio.has_value());
        CHECK(audio->samples.size() == 2);
    }

    // An Ogg file's own header. Named as a WAV, which is the case a reader trusting the
    // extension gets wrong, and the one that would then walk a Vorbis stream as RIFF chunks.
    const std::string ogg = std::string{"OggS", 4} + std::string(60, '\0');
    for (const char* name : {"music.wav", "music.ogg"}) {
        const auto audio = import_audio(bytes_of(ogg), name);
        INFO("named " << name);
        REQUIRE_FALSE(audio.has_value());
        CHECK(audio.error().code() == atlas::ErrorCode::AssetImportFailed);
    }
}

TEST_CASE("an unrecognised format says what it found", "[assets][wav]") {
    // "Unrecognised format" sends whoever reads it looking at their pipeline. The four bytes
    // that were actually there usually tell them what happened instead — most often that the
    // file is a text error page, or an Ogg they expected this engine to decode.
    const std::string ogg = std::string{"OggS", 4} + std::string(60, '\0');
    const auto audio = import_audio(bytes_of(ogg), "music.wav");
    REQUIRE_FALSE(audio.has_value());
    CHECK(audio.error().message().contains("OggS"));

    // Unprintable bytes become a placeholder rather than corrupting the log line they land in.
    const std::string binary{"\x01\x02\x03\x04", 4};
    const auto refused = import_audio(bytes_of(binary), "odd.wav");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("????"));
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/hash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using atlas::hash_bytes;
using atlas::hash_string;
using atlas::Hasher;
using atlas::to_hex;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a byte view of text.
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// splitmix64, so the quality tests below use the same inputs on every machine and every run.
/// Deliberately not the engine's RngStream: a test that generated its inputs with something
/// built on the thing under test would be worth less.
class Splitmix {
  public:
    constexpr explicit Splitmix(std::uint64_t seed) noexcept : m_state(seed) {}

    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        m_state += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31U);
    }

  private:
    std::uint64_t m_state;
};

struct Avalanche {
    double mean_bits_changed = 0.0;  ///< Of 64. Ideal is 32.
    double worst_bit_bias = 0.0;     ///< Largest |p(this output bit flips) - 0.5| over all 64.
    std::uint64_t flips = 0;
};

/// Flip every bit of every input in turn, and watch what the hash does with it.
///
/// Two numbers come out. The mean is the familiar avalanche figure: how many of the 64 output
/// bits change for a one-bit input change. The worst bias is the stronger of the two, because a
/// hash can average a respectable 32 while one particular output bit never moves at all, and an
/// output bit that never moves cannot detect change.
///
/// Enough base inputs are used to keep the sample large at every size. At four thousand flips
/// the mean of a good hash has a standard deviation of about 0.06 bits, and one bit's measured
/// probability about 0.008, so the bounds asserted below are many deviations wide. They cannot
/// flake in any case: the inputs are fixed.
[[nodiscard]] Avalanche avalanche_of(std::size_t size, std::uint64_t seed = 1) {
    constexpr std::size_t kMinimumFlips = 4096;
    const std::size_t per_base = size * 8;
    const std::size_t bases = std::max<std::size_t>(1, (kMinimumFlips + per_base - 1) / per_base);

    std::array<std::uint64_t, 64> toggles{};
    std::uint64_t changed = 0;
    std::uint64_t flips = 0;
    Splitmix random{seed};
    std::vector<std::byte> input(size);

    for (std::size_t base = 0; base < bases; ++base) {
        for (std::byte& byte : input) {
            byte = static_cast<std::byte>(random.next() & 0xFFULL);
        }
        const std::uint64_t reference = atlas::hash_bytes(input);
        for (std::size_t at = 0; at < size; ++at) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                const auto mask = static_cast<std::byte>(1U << bit);
                input[at] ^= mask;
                const std::uint64_t difference = atlas::hash_bytes(input) ^ reference;
                input[at] ^= mask;

                changed += static_cast<std::uint64_t>(std::popcount(difference));
                for (unsigned out = 0; out < 64; ++out) {
                    toggles[out] += (difference >> out) & 1ULL;
                }
                ++flips;
            }
        }
    }

    Avalanche result;
    result.flips = flips;
    result.mean_bits_changed = static_cast<double>(changed) / static_cast<double>(flips);
    for (const std::uint64_t count : toggles) {
        const double probability = static_cast<double>(count) / static_cast<double>(flips);
        result.worst_bit_bias = std::max(result.worst_bit_bias, std::abs(probability - 0.5));
    }
    return result;
}

/// The sizes that matter, and why: below thirty-two bytes only the tail path runs, which is the
/// common case for a cache key built from a few integers; thirty-two is exactly one block;
/// thirty-three is a block and a tail; the rest are several blocks.
constexpr std::array<std::size_t, 11> kQualitySizes{1, 2, 7, 8, 16, 31, 32, 33, 64, 100, 1000};

}  // namespace

TEST_CASE("the hash matches the published FNV-1a test vectors", "[core][hash]") {
    // Checked against the algorithm's own specification rather than against this
    // implementation. A hash that is only self-consistent is worthless the moment anything
    // else has to agree with it, which for asset identifiers is immediately.
    CHECK(hash_string("") == 0xCBF2'9CE4'8422'2325ULL);
    CHECK(hash_string("a") == 0xAF63'DC4C'8601'EC8CULL);
    CHECK(hash_string("foobar") == 0x85944171f73967e8ULL);
}

TEST_CASE("the hash is computable at compile time", "[core][hash]") {
    // Which is what lets an asset identifier be a constant rather than something worked out
    // at startup.
    STATIC_REQUIRE(hash_string("shaders/sprite.vert") != 0);
    STATIC_REQUIRE(hash_string("a") != hash_string("b"));
}

TEST_CASE("identifiers and content are hashed by different algorithms on purpose", "[core][hash]") {
    // Since version 2 these are two algorithms, and the split is the whole decision:
    // hash_string stays FNV-1a because its values are compile-time identifiers written into
    // saved files and gain nothing from speed, while bulk content moved to a block hash thirty
    // times faster. They must therefore disagree, and a reader who expects one to stand in for
    // the other should find out here rather than from a stale cache.
    constexpr std::string_view text = "assets/terrain.png";
    CHECK(hash_string(text) != hash_bytes(bytes_of(text)));

    // What does agree: the hasher's text path is the bulk algorithm, like every other add.
    Hasher hasher;
    hasher.add(text);
    CHECK(hasher.value() == hash_bytes(bytes_of(text)));
}

TEST_CASE("the bulk hash is computable at compile time", "[core][hash]") {
    // Not because anything hashes bytes at compile time today, but because losing it would be
    // an API change made by accident: the block loads are assembled from bytes rather than
    // copied precisely so that this keeps working.
    static constexpr std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3},
                                                    std::byte{4}};
    STATIC_REQUIRE(hash_bytes(bytes) != 0);
    STATIC_REQUIRE(hash_bytes(bytes) != hash_bytes(std::span{bytes}.subspan(0, 3)));
}

TEST_CASE("the bulk hash has not changed value", "[core][hash]") {
    // Not a published vector: this algorithm is the project's own, so this is a regression pin
    // recorded from the implementation docs/PERFORMANCE.md measured. If it fails, either the
    // algorithm changed and kHashAlgorithmVersion must change with it, or it changed by
    // accident. Long enough to cover several whole blocks and a partial one.
    constexpr std::string_view text = "the quick brown fox jumps over the lazy dog";
    CHECK(hash_bytes(bytes_of(text)) == 0xE346'1E83'8BC7'C275ULL);
}

TEST_CASE("different inputs hash differently", "[core][hash]") {
    // Not a proof of anything, but a table of realistic paths colliding would be a clear
    // sign the algorithm was misimplemented.
    const std::array<std::string_view, 8> paths{
        "shaders/sprite.vert",
        "shaders/sprite.frag",
        "shaders/triangle.vert",
        "textures/grass.png",
        "textures/grass.jpg",
        "textures/Grass.png",
        "a",
        "b",
    };

    std::set<std::uint64_t> hashes;
    for (const auto path : paths) {
        INFO("path " << path);
        CHECK(hashes.insert(hash_string(path)).second);
    }
}

TEST_CASE("case and separators change the hash", "[core][hash]") {
    // Asset paths are case-sensitive and separator-sensitive by design, so that a path which
    // differs on a case-sensitive filesystem cannot silently share an identifier.
    CHECK(hash_string("Textures/Grass.png") != hash_string("textures/grass.png"));
    CHECK(hash_string("a/b") != hash_string("a\\b"));
}

TEST_CASE("a streaming hash does not depend on where the input was split", "[core][hash]") {
    // The property that makes composing a cache key from several pieces meaningful, and the
    // one thing a block hash does not get for free: it holds partial blocks so that this stays
    // true. The old seed-chaining property, where hashing A and then B from A's result equalled
    // hashing the concatenation, is gone and cannot be recovered for a block hash.
    constexpr std::string_view whole = "abcdef";
    Hasher split;
    split.add(std::string_view{"abc"}).add(std::string_view{"def"});
    CHECK(split.value() == hash_bytes(bytes_of(whole)));
}

TEST_CASE("splitting across a block boundary does not change the hash", "[core][hash]") {
    // Thirty-two bytes is one block, so these splits land either side of one and inside the
    // buffering that exists to make them equivalent. A hash that forgot to hold partial blocks
    // would pass the six-byte case above and fail every one of these.
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text.push_back(static_cast<char>('a' + (i % 26)));
    }
    const std::uint64_t whole = hash_bytes(bytes_of(text));
    for (const std::size_t split : {std::size_t{1}, std::size_t{31}, std::size_t{32},
                                    std::size_t{33}, std::size_t{64}, std::size_t{199}}) {
        INFO("split at " << split);
        Hasher hasher;
        hasher.add(bytes_of(std::string_view{text}.substr(0, split)));
        hasher.add(bytes_of(std::string_view{text}.substr(split)));
        CHECK(hasher.value() == whole);
    }

    // And one byte at a time, which is the worst case the buffer has to survive.
    Hasher one_at_a_time;
    for (const char character : text) {
        one_at_a_time.add(std::string_view{&character, 1});
    }
    CHECK(one_at_a_time.value() == whole);
}

TEST_CASE("a one-shot hash equals a streamed one", "[core][hash]") {
    constexpr std::string_view text = "assets/terrain.png";
    Hasher hasher;
    hasher.add(bytes_of(text));
    CHECK(hash_bytes(bytes_of(text)) == hasher.value());
}

TEST_CASE("integers hash by value, not by their storage", "[core][hash]") {
    // A fixed byte order, so that a cache written on one architecture is readable on
    // another. Using the machine's own layout would make every cache appear stale on a
    // machine of the other endianness.
    Hasher a;
    a.add(std::uint32_t{0x1234'5678});

    Hasher b;
    b.add(std::uint32_t{0x1234'5678});

    Hasher different;
    different.add(std::uint32_t{0x7856'3412});

    CHECK(a.value() == b.value());
    CHECK(a.value() != different.value());
}

TEST_CASE("integer width is part of the hash", "[core][hash]") {
    // Hashing a 32-bit one and a 64-bit one identically would let two different cache keys
    // collide, which is exactly the sort of thing that shows up as a stale asset.
    Hasher narrow;
    narrow.add(std::uint32_t{1});

    Hasher wide;
    wide.add(std::uint64_t{1});

    CHECK(narrow.value() != wide.value());
}

TEST_CASE("floating-point values hash by their bits", "[core][hash]") {
    Hasher one;
    one.add(1.0F);
    Hasher same;
    same.add(1.0F);
    Hasher other;
    other.add(1.5F);

    CHECK(one.value() == same.value());
    CHECK(one.value() != other.value());

    // Positive and negative zero compare equal but are different bit patterns, and for a
    // function whose job is detecting change that difference is the honest answer.
    Hasher positive_zero;
    positive_zero.add(0.0F);
    Hasher negative_zero;
    negative_zero.add(-0.0F);
    CHECK(positive_zero.value() != negative_zero.value());
}

TEST_CASE("booleans and enumerations hash", "[core][hash]") {
    enum class Mode : std::uint8_t { First, Second };

    Hasher first;
    first.add(Mode::First);
    Hasher second;
    second.add(Mode::Second);
    CHECK(first.value() != second.value());

    Hasher yes;
    yes.add(true);
    Hasher no;
    no.add(false);
    CHECK(yes.value() != no.value());
}

TEST_CASE("order matters", "[core][hash]") {
    // Which is what makes a cache key built from several fields meaningful: swapping the
    // width and the height of a texture must not produce the same key.
    Hasher forwards;
    forwards.add(std::uint32_t{16}).add(std::uint32_t{32});

    Hasher backwards;
    backwards.add(std::uint32_t{32}).add(std::uint32_t{16});

    CHECK(forwards.value() != backwards.value());
}

TEST_CASE("the bulk hash avalanches at every size", "[core][hash][quality]") {
    // Detecting change is the entire job of a state hash, so this is a correctness property and
    // belongs here rather than only in a benchmark. It is also what caught the trap recorded in
    // docs/PERFORMANCE.md: the unfinalised version of this algorithm was twice as fast per byte
    // and scored 16.6 here, and would otherwise have been adopted as an improvement.
    for (const std::size_t size : kQualitySizes) {
        const Avalanche measured = avalanche_of(size);
        INFO("size " << size << ", " << measured.flips << " flips, mean "
                     << measured.mean_bits_changed << ", worst bit bias "
                     << measured.worst_bit_bias);
        CHECK(measured.mean_bits_changed > 31.0);
        CHECK(measured.mean_bits_changed < 33.0);
    }
}

TEST_CASE("every output bit of the bulk hash responds to every input bit",
          "[core][hash][quality]") {
    // Stronger than the mean above, and the reason both are here: a hash can average a
    // respectable thirty-two while one output bit is stuck, and a stuck bit cannot detect
    // change. Checked at every size, because the tail path and the block path are different
    // code and a stuck bit in either is equally useless.
    for (const std::size_t size : kQualitySizes) {
        const Avalanche measured = avalanche_of(size);
        INFO("size " << size << ", worst bit bias " << measured.worst_bit_bias);

        // Two bounds, because the estimate is not equally good at both ends. This statistic is
        // the largest deviation across all sixty-four output bits, and at one and two bytes
        // there are only 256 and 65536 possible inputs, so the sample repeats itself and the
        // maximum wanders further. Observed: 0.047 at one byte, 0.019 at two, and never above
        // 0.027 from eight bytes up. The bounds are set to leave real margin over those rather
        // than to sit just above them.
        CHECK(measured.worst_bit_bias < (size < 8 ? 0.07 : 0.04));
    }
}

TEST_CASE("the bulk hash does not collide on structured inputs", "[core][hash][quality]") {
    // Real data is not random: it is sequential counters, sparse bit patterns, and paths that
    // differ by one character. Those are the shapes a weak hash fails on, and a 64-bit hash
    // should produce no collision at all across a corpus this size — the birthday estimate for
    // a quarter of a million values is about two in a thousand million.
    //
    // A hash set is the right structure here and the ordering rule permits it: nothing iterates
    // it, and inserting and looking up is all that happens.
    std::unordered_set<std::uint64_t> seen;
    std::size_t inputs = 0;
    const auto record = [&seen, &inputs](std::uint64_t hash) {
        ++inputs;
        return seen.insert(hash).second;
    };

    // Each corpus is tagged with a leading byte so that every input across all three is
    // distinct. Without it the sparse patterns below collide with the counters by construction
    // — the pattern 0b11 is the number three — and the test would be reporting its own
    // overlap as a hash failure, which is exactly what it did when first written.
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        Hasher hasher;
        hasher.add(std::uint8_t{0}).add(i);
        REQUIRE(record(hasher.value()));
    }

    // Every one and two bit pattern in eight bytes: inputs that differ as little as possible.
    for (unsigned first = 0; first < 64; ++first) {
        for (unsigned second = first; second < 64; ++second) {
            Hasher hasher;
            hasher.add(std::uint8_t{1}).add((1ULL << first) | (1ULL << second));
            REQUIRE(record(hasher.value()));
        }
    }

    // Paths that differ by one character, which is what an asset tree looks like.
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        const std::string path = "textures/terrain_" + std::to_string(i) + ".png";
        Hasher hasher;
        hasher.add(std::uint8_t{2}).add(bytes_of(path));
        REQUIRE(record(hasher.value()));
    }

    CHECK(seen.size() == inputs);
    CHECK(inputs > 200'000);
}

TEST_CASE("words that fall in different lanes are not interchangeable", "[core][hash][quality]") {
    // The block path keeps four independent chains, each taking one word of every thirty-two
    // byte block. Two words in different lanes must not be swappable without changing the
    // answer, and neither must two in the same lane in different blocks. Lanes that were merely
    // exclusive-ored together at the end, rather than seeded apart and mixed by position, would
    // fail the first of these.
    std::vector<std::byte> input(64);
    Splitmix random{7};
    for (std::byte& byte : input) {
        byte = static_cast<std::byte>(random.next() & 0xFFULL);
    }
    const std::uint64_t reference = hash_bytes(input);

    const auto swap_words = [&input](std::size_t left, std::size_t right) {
        std::swap_ranges(input.begin() + static_cast<std::ptrdiff_t>(left * 8),
                         input.begin() + static_cast<std::ptrdiff_t>((left + 1) * 8),
                         input.begin() + static_cast<std::ptrdiff_t>(right * 8));
    };

    swap_words(0, 1);  // lane zero with lane one, inside one block
    CHECK(hash_bytes(input) != reference);
    swap_words(0, 1);
    REQUIRE(hash_bytes(input) == reference);

    swap_words(0, 4);  // lane zero of the first block with lane zero of the second
    CHECK(hash_bytes(input) != reference);
    swap_words(0, 4);
    REQUIRE(hash_bytes(input) == reference);
}

TEST_CASE("identifier hashing is not held to the same standard, on purpose",
          "[core][hash][quality]") {
    // hash_string is FNV-1a and stays that way for compatibility, not because it is the better
    // hash: it avalanches at about 30.7 of 64 where the bulk hash manages 32.0. What it has to
    // do is not collide across a corpus of names, because names are what identifiers are, so
    // that is what is checked. Holding it to the bulk hash's thresholds would be asserting
    // something the project has deliberately chosen not to fix.
    std::unordered_set<std::uint64_t> seen;
    std::size_t names = 0;
    for (const std::string_view prefix : {"textures/", "shaders/", "scenes/", "audio/"}) {
        for (std::uint64_t i = 0; i < 25'000; ++i) {
            const std::string name = std::string{prefix} + "asset_" + std::to_string(i);
            ++names;
            REQUIRE(seen.insert(hash_string(name)).second);
        }
    }
    CHECK(seen.size() == names);
}

TEST_CASE("hex formatting is fixed width and lowercase", "[core][hash]") {
    CHECK(to_hex(0) == "0000000000000000");
    CHECK(to_hex(1) == "0000000000000001");
    CHECK(to_hex(0xDEAD'BEEF'CAFE'1234ULL) == "deadbeefcafe1234");
    CHECK(to_hex(0xFFFF'FFFF'FFFF'FFFFULL) == "ffffffffffffffff");
    CHECK(to_hex(42).size() == 16);
}

TEST_CASE("the algorithm version is recorded", "[core][hash]") {
    // Stored alongside a hash, so that changing the algorithm later is detected rather than
    // mistaken for every asset having changed at once.
    // Version 2: FNV-1a for identifiers, the four-lane block hash for bulk content.
    STATIC_REQUIRE(atlas::kHashAlgorithmVersion == 2);
}

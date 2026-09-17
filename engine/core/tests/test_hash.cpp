// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/hash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <string_view>

using atlas::hash_bytes;
using atlas::hash_string;
using atlas::Hasher;
using atlas::to_hex;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a byte view of text.
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

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

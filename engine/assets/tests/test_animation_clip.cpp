// SPDX-License-Identifier: GPL-3.0-or-later
// The animation clip reader, driven directly, with every input built here.
//
// Nothing on disk, following the image and audio tests: an input assembled by the test is one
// the test can make hostile on purpose, one field at a time.
//
// What "hostile" means here differs from the audio reader and the difference is the point. That
// reader refuses a declared size before it allocates. A document parser cannot — by the time any
// count is readable the whole document is in memory — so the bound that matters is on the input
// text itself, and everything after the parse is about refusing a document that means something
// impossible.
#include "../src/animation_clip.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using atlas::assets::detail::parse_animation_clip;
using Catch::Approx;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string& text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

[[nodiscard]] auto parse(const std::string& text) {
    return parse_animation_clip(bytes_of(text), "test.clip.json");
}

/// A whole document with the body given. Everything a valid clip needs and nothing else, so a
/// case can replace exactly the part it is about.
[[nodiscard]] std::string document(std::string_view body) {
    return std::string{R"({"format": "atlas-animation-clip", "version": 1, )"} + std::string{body} +
           "}";
}

constexpr std::string_view kMinimalTransform =
    R"("duration_ms": 1000, "transform": [{"time_ms": 0}, {"time_ms": 1000, "x": 10.0}])";

}  // namespace

TEST_CASE("a well-formed clip decodes to the keys it holds", "[assets][clip]") {
    const auto clip = parse(document(
        R"("name": "orbit", "duration_ms": 4000,
            "grid": {"columns": 4, "rows": 2},
            "transform": [
              {"time_ms": 0, "x": 0.0, "y": 0.0, "rotation": 0.0, "easing": "cubic-in-out"},
              {"time_ms": 4000, "x": 30.0, "y": -18.0, "rotation": 6.28318, "scale_x": 2.0}
            ],
            "frames": [{"time_ms": 0, "cell": 0}, {"time_ms": 2000, "cell": 7}])"));
    REQUIRE(clip.has_value());

    CHECK(clip->name == "orbit");
    // Milliseconds in the file, nanoseconds in memory. The conversion happens once, here.
    CHECK(clip->duration_ns == 4'000'000'000ULL);
    CHECK(clip->columns == 4);
    CHECK(clip->rows == 2);

    REQUIRE(clip->transform_keys.size() == 2);
    CHECK(clip->transform_keys[0].time_ns == 0);
    // Checked by value, not only by count: a parser reading the wrong field produces exactly
    // the right number of keys and the wrong motion.
    CHECK(clip->transform_keys[1].position_x == Approx(30.0F));
    CHECK(clip->transform_keys[1].position_y == Approx(-18.0F));
    CHECK(clip->transform_keys[1].scale_x == Approx(2.0F));
    // Defaulted rather than absent: a key that says nothing about scale means "unchanged",
    // which is one and not zero.
    CHECK(clip->transform_keys[1].scale_y == Approx(1.0F));
    // Easings agree with the animation module by position, and this is the only place that is
    // true. cubic-in-out is the last of the eight.
    CHECK(clip->transform_keys[0].easing == 7);
    CHECK(clip->transform_keys[1].easing == 0);

    REQUIRE(clip->frame_keys.size() == 2);
    CHECK(clip->frame_keys[1].time_ns == 2'000'000'000ULL);
    CHECK(clip->frame_keys[1].cell == 7);
}

TEST_CASE("a clip may have only one track", "[assets][clip]") {
    const auto frames_only = parse(document(
        R"("duration_ms": 500, "grid": {"columns": 2, "rows": 1},
            "frames": [{"time_ms": 0, "cell": 0}])"));
    REQUIRE(frames_only.has_value());
    CHECK(frames_only->transform_keys.empty());
    CHECK(frames_only->frame_keys.size() == 1);

    const auto transform_only = parse(document(kMinimalTransform));
    REQUIRE(transform_only.has_value());
    CHECK(transform_only->frame_keys.empty());
}

TEST_CASE("a clip with comments in it is read", "[assets][clip]") {
    // A clip is a file somebody tunes by hand, and a note beside a key is what they would
    // write. The scene reader accepts comments for the same reason.
    const auto clip = parse(R"({
  "format": "atlas-animation-clip",
  "version": 1,
  // four seconds, one revolution
  "duration_ms": 4000,
  "transform": [{"time_ms": 0}, {"time_ms": 4000, "rotation": 6.28318}]
})");
    CHECK(clip.has_value());
}

// -------------------------------------------------------------- hostile input from here on

TEST_CASE("a document larger than the cap is refused before it is parsed", "[assets][clip]") {
    // The one bound a document parser can actually enforce. Once the text has been handed to
    // the parser it has already been allocated, so an enormous input has to be refused here or
    // not at all.
    const std::string enormous(2ULL * 1024 * 1024, 'x');
    const auto clip = parse(enormous);
    REQUIRE_FALSE(clip.has_value());
    CHECK(clip.error().code() == atlas::ErrorCode::MalformedData);
    CHECK(clip.error().message().contains("past the limit"));
}

TEST_CASE("a document that is not a clip is refused", "[assets][clip]") {
    CHECK_FALSE(parse("").has_value());
    CHECK_FALSE(parse("this is not JSON at all").has_value());
    CHECK_FALSE(parse("[1, 2, 3]").has_value());
    // Valid JSON, wrong thing. The marker is the claim a file has to honour to be read at all.
    CHECK_FALSE(parse(R"({"format": "atlas-scene", "version": 1})").has_value());
    CHECK_FALSE(parse(R"({"version": 1, "duration_ms": 100})").has_value());
}

TEST_CASE("a version this build cannot read is refused", "[assets][clip]") {
    const auto newer = parse(
        R"({"format": "atlas-animation-clip", "version": 2, "duration_ms": 100,
            "transform": [{"time_ms": 0}]})");
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().message().contains("newer build"));

    CHECK_FALSE(parse(R"({"format": "atlas-animation-clip", "version": 0})").has_value());
}

TEST_CASE("a clip that animates nothing is refused", "[assets][clip]") {
    // Neither track. Accepting it would mean an entity that carries an animator, resolves a
    // clip, and never moves — which looks exactly like a bug somewhere else.
    CHECK_FALSE(parse(document(R"("duration_ms": 1000)")).has_value());
}

TEST_CASE("a duration that is not one is refused", "[assets][clip]") {
    CHECK_FALSE(parse(document(R"("transform": [{"time_ms": 0}])")).has_value());
    CHECK_FALSE(parse(document(R"("duration_ms": 0, "transform": [{"time_ms": 0}])")).has_value());
    // Longer than a day.
    CHECK_FALSE(
        parse(document(R"("duration_ms": 999999999, "transform": [{"time_ms": 0}])")).has_value());
}

TEST_CASE("keys out of order are refused", "[assets][clip]") {
    // The claim the animation module's own header already made and nothing enforced. Sampling
    // binary-searches the keys, so an unordered track does not fail — it silently returns the
    // wrong pose, which is the worst of the available failures.
    CHECK_FALSE(parse(document(R"("duration_ms": 1000,
        "transform": [{"time_ms": 0}, {"time_ms": 800}, {"time_ms": 400}])"))
                    .has_value());

    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "grid": {"columns": 4, "rows": 1},
        "frames": [{"time_ms": 0, "cell": 0}, {"time_ms": 900, "cell": 1},
                   {"time_ms": 100, "cell": 2}])"))
                    .has_value());
}

TEST_CASE("a track that does not start at zero is refused", "[assets][clip]") {
    // The other claim the header made. A track starting late is not undefined for the time
    // before it — the sampler holds the first key — but a clip that meant to start late should
    // say so with a key at zero rather than by omission.
    CHECK_FALSE(parse(document(R"("duration_ms": 1000,
        "transform": [{"time_ms": 100}, {"time_ms": 1000}])"))
                    .has_value());
}

TEST_CASE("a key past the clip's own duration is refused", "[assets][clip]") {
    CHECK_FALSE(parse(document(R"("duration_ms": 1000,
        "transform": [{"time_ms": 0}, {"time_ms": 5000}])"))
                    .has_value());
}

TEST_CASE("a cell the grid does not have is refused", "[assets][clip]") {
    // The sampler wraps, because showing the wrong frame beats reading outside a texture. A
    // clip that names a cell its own grid has not is a clip disagreeing with itself, and that
    // is worth saying here rather than drawing.
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "grid": {"columns": 2, "rows": 2},
        "frames": [{"time_ms": 0, "cell": 4}])"))
                    .has_value());

    // And the default grid is one cell, so anything but zero is out of range without one.
    CHECK_FALSE(parse(document(R"("duration_ms": 1000,
        "frames": [{"time_ms": 0, "cell": 1}])"))
                    .has_value());
}

TEST_CASE("an impossible grid is refused", "[assets][clip]") {
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "grid": {"columns": 0, "rows": 2},
        "frames": [{"time_ms": 0, "cell": 0}])"))
                    .has_value());
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "grid": {"columns": 99999, "rows": 2},
        "frames": [{"time_ms": 0, "cell": 0}])"))
                    .has_value());
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "grid": {"columns": 2},
        "frames": [{"time_ms": 0, "cell": 0}])"))
                    .has_value());
}

TEST_CASE("an easing this build does not know is refused", "[assets][clip]") {
    // Guessing would play a clip differently from what its file says while looking as though
    // it worked, which is the failure that takes longest to find.
    CHECK_FALSE(parse(document(R"("duration_ms": 1000,
        "transform": [{"time_ms": 0, "easing": "bouncy"}])"))
                    .has_value());
}

TEST_CASE("a value that is not finite is refused", "[assets][clip]") {
    // JSON has no syntax for infinity, so this cannot arrive directly. A number large enough
    // to become infinity when narrowed to a float can, and an infinite offset makes every
    // matrix it touches meaningless rather than merely wrong.
    CHECK_FALSE(parse(document(R"("duration_ms": 1000,
        "transform": [{"time_ms": 0, "x": 1e40}])"))
                    .has_value());
    CHECK_FALSE(parse(document(R"("duration_ms": 1000,
        "transform": [{"time_ms": 0, "x": "over there"}])"))
                    .has_value());
}

TEST_CASE("a malformed key is refused rather than skipped", "[assets][clip]") {
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "transform": [42])")).has_value());
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "transform": [{"x": 1.0}])")).has_value());
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "transform": "soon")")).has_value());
    CHECK_FALSE(parse(document(R"("duration_ms": 1000, "grid": 4,
        "frames": [{"time_ms": 0, "cell": 0}])"))
                    .has_value());
}

TEST_CASE("a truncated document is refused at every length", "[assets][clip]") {
    // Cut at every length rather than at one chosen point: the interesting cuts are the ones
    // inside a token, and picking them by hand means picking the ones already thought of.
    const std::string whole = document(kMinimalTransform);
    for (std::size_t length = 0; length < whole.size(); ++length) {
        const std::string cut = whole.substr(0, length);
        INFO("cut at " << length);
        CHECK_FALSE(parse(cut).has_value());
    }
    CHECK(parse(whole).has_value());
}

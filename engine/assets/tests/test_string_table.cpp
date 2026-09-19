// SPDX-License-Identifier: GPL-3.0-or-later
// The string table reader, driven directly, on inputs built here.
//
// Every document below is written in this file rather than committed, which is the convention
// the image and audio tests already follow: a fixture nobody can read is a fixture nobody
// maintains. What is being asserted is uniform — **a malformed table produces an error naming
// what was wrong, never a partial table and never a crash** — and the suite is run under the
// address sanitiser, where a parser that walked off the end of a truncated document would say
// so rather than usually getting away with it.
#include "../src/string_table.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

using atlas::assets::detail::parse_string_table;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] auto parse(std::string_view text) {
    return parse_string_table(bytes_of(text), "test.json");
}

constexpr std::string_view kMinimal = R"({
  "format": "atlas-strings",
  "version": 1,
  "locale": "en",
  "strings": {"ui.ok": "OK", "ui.cancel": "Cancel"}
})";

}  // namespace

TEST_CASE("a well-formed table parses", "[assets]") {
    const auto table = parse(kMinimal);
    REQUIRE(table.has_value());

    CHECK(table->locale == "en");
    REQUIRE(table->strings.size() == 2);

    // The document's own order is kept. Nothing depends on it, and it is kept so that an error
    // can name the entry a reader would have been looking at.
    CHECK(table->strings[0].first == "ui.ok");
    CHECK(table->strings[0].second == "OK");
    CHECK(table->strings[1].first == "ui.cancel");
}

TEST_CASE("comments are permitted, as in the scene and clip files", "[assets]") {
    // A table is a file somebody edits by hand, and a note beside an entry is exactly what they
    // would write. The committed English table uses this.
    const auto table = parse(R"({
      // which language this is
      "format": "atlas-strings",
      "version": 1,
      "locale": "en",
      "strings": {"ui.ok": "OK"}
    })");
    REQUIRE(table.has_value());
    CHECK(table->strings.size() == 1);
}

TEST_CASE("an empty table is legitimate", "[assets]") {
    const auto table = parse(R"({
      "format": "atlas-strings", "version": 1, "locale": "en", "strings": {}
    })");
    REQUIRE(table.has_value());
    CHECK(table->strings.empty());
    CHECK(table->locale == "en");
}

TEST_CASE("an empty value is a deliberate blank, not a malformed entry", "[assets]") {
    const auto table = parse(R"({
      "format": "atlas-strings", "version": 1, "locale": "en",
      "strings": {"ui.blank": ""}
    })");
    REQUIRE(table.has_value());
    REQUIRE(table->strings.size() == 1);
    CHECK(table->strings[0].second.empty());
}

TEST_CASE("a table that is not JSON is refused", "[assets]") {
    CHECK_FALSE(parse("").has_value());
    CHECK_FALSE(parse("not json at all").has_value());
    CHECK_FALSE(parse("[1, 2, 3]").has_value());
    CHECK_FALSE(parse(R"({"format": "atlas-strings")").has_value());
}

TEST_CASE("a truncated document is refused at every length", "[assets]") {
    // Cut at every byte, because a parser that reads one past a buffer usually gets away with
    // it and this is the shape that finds out. Under ASan this is the case that matters most.
    for (std::size_t length = 0; length < kMinimal.size(); ++length) {
        const auto table = parse(kMinimal.substr(0, length));
        CHECK_FALSE(table.has_value());
    }
    CHECK(parse(kMinimal).has_value());
}

TEST_CASE("a document that is not a string table is refused", "[assets]") {
    // Named, not guessed. An animation clip is valid JSON and would otherwise parse to an empty
    // table, which is the failure that looks like success.
    const auto table = parse(R"({
      "format": "atlas-animation-clip", "version": 1, "duration_ms": 100
    })");
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().code() == atlas::ErrorCode::MalformedData);
}

TEST_CASE("a newer version is refused rather than half-understood", "[assets]") {
    const auto table = parse(R"({
      "format": "atlas-strings", "version": 2, "locale": "en", "strings": {}
    })");
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().to_string().contains("newer build"));
}

TEST_CASE("a table with no version is refused", "[assets]") {
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "locale": "en", "strings": {}})").has_value());
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "version": "1", "locale": "en",
                          "strings": {}})")
                    .has_value());
}

TEST_CASE("a table must say which locale it is for", "[assets]") {
    // Defaulting this to English would make the one mistake that matters invisible: a French
    // table filed as English shows French text and reports itself as English.
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "version": 1, "strings": {}})").has_value());
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "version": 1, "locale": "", "strings": {}})")
                    .has_value());
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "version": 1, "locale": 7, "strings": {}})")
                    .has_value());
}

TEST_CASE("a table with no strings object is refused", "[assets]") {
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "version": 1, "locale": "en"})").has_value());
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "version": 1, "locale": "en", "strings": []})")
                    .has_value());
}

TEST_CASE("a duplicate key is refused rather than resolved", "[assets]") {
    // Which of two identical keys should win is not a question a translator can be expected to
    // answer, and resolving it silently would make the interface depend on parse order.
    const auto table = parse(R"({
      "format": "atlas-strings", "version": 1, "locale": "en",
      "strings": {"ui.ok": "OK", "ui.ok": "Okay"}
    })");
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().to_string().contains("more than once"));
}

TEST_CASE("a non-string value is refused and names its key", "[assets]") {
    const auto table = parse(R"({
      "format": "atlas-strings", "version": 1, "locale": "en",
      "strings": {"ui.count": 7}
    })");
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().to_string().contains("ui.count"));
}

TEST_CASE("an empty key is refused", "[assets]") {
    CHECK_FALSE(parse(R"({"format": "atlas-strings", "version": 1, "locale": "en",
                          "strings": {"": "nothing"}})")
                    .has_value());
}

TEST_CASE("an oversized key is refused", "[assets]") {
    const std::string key(atlas::assets::import_limits().max_string_key_length + 1, 'k');
    const auto table = parse(R"({"format": "atlas-strings", "version": 1, "locale": "en",
                                 "strings": {")" +
                             key + R"(": "v"}})");
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().to_string().contains("past the limit"));
}

TEST_CASE("an oversized value is refused", "[assets]") {
    const std::string value(atlas::assets::import_limits().max_string_value_length + 1, 'v');
    const auto table = parse(R"({"format": "atlas-strings", "version": 1, "locale": "en",
                                 "strings": {"ui.long": ")" +
                             value + R"("}})");
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().to_string().contains("past the limit"));
}

TEST_CASE("a document past the size cap is refused before it is parsed", "[assets]") {
    // **Padded with a comment, and that is the point of the test rather than a trick.** The
    // bound is on the input text itself, before the parse, so the document that exercises it
    // must be one where nothing else is wrong: every key, every value and the entry count are
    // all well inside their own limits, and the only thing past a limit is the number of bytes.
    //
    // A first attempt padded one value with megabytes of 'x' instead. That is refused by the
    // value-length check whether the size cap exists or not, so it passed with the cap removed
    // and proved nothing. The mutation run is what found that.
    std::string document = R"({"format": "atlas-strings", "version": 1, "locale": "en",
                               // )";
    document.append(atlas::assets::import_limits().max_string_table_bytes, 'x');
    document += R"(
                               "strings": {"ui.ok": "OK"}})";

    const auto table = parse(document);
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().to_string().contains("past the limit"));

    // The same document without the padding is fine, so the padding is what was refused.
    CHECK(parse(R"({"format": "atlas-strings", "version": 1, "locale": "en",
                    "strings": {"ui.ok": "OK"}})")
              .has_value());
}

TEST_CASE("a table declaring more entries than the limit is refused", "[assets]") {
    std::string document = R"({"format": "atlas-strings", "version": 1, "locale": "en",
                               "strings": {)";
    const std::uint32_t count = atlas::assets::import_limits().max_strings + 1;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i > 0) {
            document += ',';
        }
        document += '"';
        document += "k" + std::to_string(i);
        document += R"(": "v")";
    }
    document += "}}";

    const auto table = parse(document);
    REQUIRE_FALSE(table.has_value());
    CHECK(table.error().to_string().contains("past the limit"));
}

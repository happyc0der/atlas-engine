// SPDX-License-Identifier: GPL-3.0-or-later
// The catalog, filled in code rather than from a file.
//
// The asset path arrives in the next slice; everything asserted here is about the lookup
// itself, and the case that matters most is the one where the lookup fails — because that is
// the path a shipped build takes when somebody adds a string and forgets the table.
#include <atlas/assets/importer.hpp>
#include <atlas/text/catalog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using atlas::text::Catalog;

TEST_CASE("a key that was inserted is found", "[text]") {
    Catalog catalog;
    REQUIRE(catalog.insert("ui.controls.pause", "Pause").has_value());

    CHECK(catalog.lookup("ui.controls.pause") == "Pause");
    CHECK(catalog.size() == 1);
    CHECK(catalog.misses() == 0);
}

TEST_CASE("a missing key returns itself and is counted", "[text]") {
    Catalog catalog;

    // Not an error and not empty: a panel showing `ui.controls.pause` is visibly wrong and
    // still usable, and it names the key somebody has to add.
    CHECK(catalog.lookup("ui.controls.pause") == "ui.controls.pause");
    CHECK(catalog.misses() == 1);
    CHECK(catalog.distinct_misses() == 1);
}

TEST_CASE("a repeated miss is counted every time and recorded once", "[text]") {
    Catalog catalog;

    for (int i = 0; i < 100; ++i) {
        CHECK(catalog.lookup("ui.missing") == "ui.missing");
    }

    // The distinction is the whole point of remembering keys: the count says how often the
    // interface drew a hole, and the distinct count says how many strings somebody must write.
    CHECK(catalog.misses() == 100);
    CHECK(catalog.distinct_misses() == 1);

    // And it is named once, however many frames asked.
    CHECK(catalog.log_new_misses() == 1);
    CHECK(catalog.log_new_misses() == 0);
}

TEST_CASE("a key is named once and never again", "[text]") {
    Catalog catalog;

    (void)catalog.lookup("ui.a");
    (void)catalog.lookup("ui.b");
    CHECK(catalog.log_new_misses() == 2);

    // Nothing new since the last call.
    CHECK(catalog.log_new_misses() == 0);

    (void)catalog.lookup("ui.a");
    (void)catalog.lookup("ui.c");
    CHECK(catalog.log_new_misses() == 1);
}

TEST_CASE("nothing is reported when nothing was missed", "[text]") {
    Catalog catalog;
    REQUIRE(catalog.insert("ui.ok", "OK").has_value());
    CHECK(catalog.lookup("ui.ok") == "OK");
    CHECK(catalog.log_new_misses() == 0);
}

TEST_CASE("recorded misses are bounded", "[text]") {
    Catalog catalog;

    // A caller that builds keys in a loop must not make the catalog grow without limit, which
    // is a leak that only appears when something is already going wrong.
    const std::size_t attempts = Catalog::kMaxRecordedMisses + 500;
    for (std::size_t i = 0; i < attempts; ++i) {
        const std::string key = "ui.generated." + std::to_string(i);
        CHECK(catalog.lookup(key) == key);
    }

    CHECK(catalog.misses() == attempts);
    CHECK(catalog.distinct_misses() == Catalog::kMaxRecordedMisses);

    // Everything recorded is still named exactly once, and the keys past the bound are counted
    // without being remembered.
    CHECK(catalog.log_new_misses() == Catalog::kMaxRecordedMisses);
    CHECK(catalog.log_new_misses() == 0);
}

TEST_CASE("a duplicate key is refused rather than replaced", "[text]") {
    Catalog catalog;
    REQUIRE(catalog.insert("ui.ok", "OK").has_value());

    const auto second = catalog.insert("ui.ok", "Okay");
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code() == atlas::ErrorCode::MalformedData);

    // The first entry stands. Which of two identical keys wins is not a question a translator
    // can answer, so the file is refused instead of being resolved by parse order.
    CHECK(catalog.lookup("ui.ok") == "OK");
    CHECK(catalog.size() == 1);
}

TEST_CASE("an empty key is refused", "[text]") {
    Catalog catalog;
    const auto status = catalog.insert("", "something");
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == atlas::ErrorCode::MalformedData);
}

TEST_CASE("an empty value is a legitimate entry", "[text]") {
    Catalog catalog;
    REQUIRE(catalog.insert("ui.blank", "").has_value());

    // A translator deliberately blanking a string is different from a key that is missing, and
    // the two must not report the same thing: this is a hit, so it is not counted as a miss.
    CHECK(catalog.lookup("ui.blank").empty());
    CHECK(catalog.misses() == 0);
}

TEST_CASE("clear drops the entries and the memory of what was missing", "[text]") {
    Catalog catalog;
    REQUIRE(catalog.insert("ui.ok", "OK").has_value());
    (void)catalog.lookup("ui.absent");

    catalog.clear();

    CHECK(catalog.size() == 0);
    CHECK(catalog.misses() == 0);
    CHECK(catalog.distinct_misses() == 0);

    // The unreported miss goes with it: a key recorded against the old table must not be
    // named against the new one as though it had just been asked for.
    CHECK(catalog.log_new_misses() == 0);

    // A key absent from the old table may be present in the new one, so it is asked about
    // again rather than remembered as hopeless.
    CHECK(catalog.lookup("ui.ok") == "ui.ok");
}

TEST_CASE("a failed load leaves the catalog empty, not half-filled", "[text]") {
    // `load` is reachable without a parser — the lab calls it directly — so its own failure
    // path needs its own test. Built here rather than parsed, because the importer refuses a
    // duplicate first and this is about what happens when one reaches the catalog anyway.
    Catalog catalog;
    REQUIRE(catalog.insert("ui.previous", "Previous").has_value());

    atlas::assets::ImportedStringTable table;
    table.locale = "en";
    table.strings.emplace_back("ui.first", "First");
    table.strings.emplace_back("ui.second", "Second");
    table.strings.emplace_back("ui.first", "Again");

    const auto status = catalog.load(table);
    REQUIRE_FALSE(status.has_value());

    // Neither the old contents nor the half that was accepted: a refused table leaves nothing
    // of itself behind, and nothing of what it replaced either.
    CHECK(catalog.size() == 0);
    CHECK(catalog.locale().empty());
    CHECK(catalog.lookup("ui.first") == "ui.first");
    CHECK(catalog.lookup("ui.previous") == "ui.previous");
}

TEST_CASE("load replaces rather than merges", "[text]") {
    Catalog catalog;
    REQUIRE(catalog.insert("ui.gone", "Gone").has_value());

    atlas::assets::ImportedStringTable table;
    table.locale = "fr";
    table.strings.emplace_back("ui.kept", "Gardé");

    REQUIRE(catalog.load(table).has_value());
    CHECK(catalog.size() == 1);
    CHECK(catalog.locale() == "fr");
    CHECK(catalog.lookup("ui.kept") == "Gardé");
    CHECK(catalog.lookup("ui.gone") == "ui.gone");
}

TEST_CASE("a key found on a hit stays valid as more entries arrive", "[text]") {
    Catalog catalog;
    REQUIRE(catalog.insert("ui.first", "First").has_value());
    const std::string_view held = catalog.lookup("ui.first");

    for (int i = 0; i < 1000; ++i) {
        REQUIRE(catalog.insert("ui.filler." + std::to_string(i), "x").has_value());
    }

    // The container is node-based, so rehashing moves nodes between buckets and never moves
    // the strings themselves. The header promises this; without it every caller holding a
    // looked-up view across an insert would be reading freed memory.
    CHECK(held == "First");
}

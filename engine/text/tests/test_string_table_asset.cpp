// SPDX-License-Identifier: GPL-3.0-or-later
// A table from a file on disk, through the registry, into the catalog.
//
// The parser is tested on its own and so is the catalog. Neither working alone implies this:
// the parser could produce a shape the catalog loads wrongly, and the catalog could load
// correctly something the parser never emits. What this covers is the seam between them, and
// the one thing neither can show — **that an asset which decodes is actually claimed**, and so
// does not sit in the registry being reported as stalled, which is what a missing finaliser
// looks like and what AssetType::Shader has been doing since M4.
#include <atlas/assets/filesystem.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/text/catalog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using atlas::assets::AssetState;
using atlas::assets::AssetType;
using atlas::assets::FileSystem;
using atlas::assets::Registry;
using atlas::assets::VirtualPath;
using atlas::text::Catalog;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// Distinct rather than identity: what is wanted is that two trees do not collide, and an
/// address would say the wrong thing. Same reasoning as the registry's own fixture.
[[nodiscard]] int next_scratch_id() {
    static std::atomic<int> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

class TempTree {
  public:
    TempTree() {
        m_root = std::filesystem::temp_directory_path() /
                 ("atlas-strings-" + std::to_string(next_scratch_id()));
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    void write(std::string_view relative, std::string_view contents) const {
        const auto path = m_root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

  private:
    std::filesystem::path m_root;
};

constexpr std::string_view kEnglish = R"({
  "format": "atlas-strings", "version": 1, "locale": "en",
  "strings": {"ui.ok": "OK", "ui.cancel": "Cancel"}
})";

/// Deliberately fewer keys than the first, and one of them changed.
///
/// Fewer because that is what proves a reload replaces rather than merges: a table that kept
/// `ui.cancel` after it was deleted from the file would still answer for it, and the interface
/// would go on showing a string that no longer exists anywhere.
constexpr std::string_view kEdited = R"({
  "format": "atlas-strings", "version": 1, "locale": "en",
  "strings": {"ui.ok": "Right then"}
})";

/// Pump and finalise until the asset settles, or give up. A bounded wait rather than a sleep:
/// how long a worker takes is not something a test should assert about.
[[nodiscard]] bool settle(Registry& registry, Catalog& catalog, atlas::assets::AssetId id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        registry.pump();
        catalog.finalise_pending(registry);
        const auto info = registry.info(id);
        if (info && (info->state == AssetState::Ready || info->state == AssetState::Failed)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a table loads through the registry and is claimed", "[text]") {
    REQUIRE(kMainThreadMarked);

    const TempTree tree;
    tree.write("strings/en.json", kEnglish);

    FileSystem filesystem;
    REQUIRE(filesystem.mount("/", tree.root(), 0).has_value());

    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());
    Catalog catalog;

    const auto path = VirtualPath::parse("strings/en.json");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::StringTable);
    REQUIRE(id.has_value());

    REQUIRE(settle(*registry, catalog, *id));

    const auto info = registry->info(*id);
    REQUIRE(info.has_value());
    CHECK(info->state == AssetState::Ready);

    CHECK(catalog.size() == 2);
    CHECK(catalog.locale() == "en");
    CHECK(catalog.lookup("ui.ok") == "OK");
    CHECK(catalog.lookup("ui.cancel") == "Cancel");

    // Nothing is left decoded-and-unclaimed, which is the state a missing finaliser produces
    // and which the registry reports about ten seconds later.
    CHECK(registry->stats().awaiting_finalisation == 0);
}

TEST_CASE("a malformed table fails the asset rather than half-loading it", "[text]") {
    REQUIRE(kMainThreadMarked);

    const TempTree tree;
    tree.write("strings/broken.json", R"({"format": "atlas-strings", "version": 99})");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("/", tree.root(), 0).has_value());

    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());
    Catalog catalog;

    const auto path = VirtualPath::parse("strings/broken.json");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::StringTable);
    REQUIRE(id.has_value());

    REQUIRE(settle(*registry, catalog, *id));

    const auto info = registry->info(*id);
    REQUIRE(info.has_value());
    CHECK(info->state == AssetState::Failed);

    // The catalog is untouched: a refused table leaves no half of itself behind.
    CHECK(catalog.size() == 0);
    CHECK(catalog.locale().empty());
    CHECK(registry->stats().awaiting_finalisation == 0);
}

TEST_CASE("a reload replaces the table rather than merging into it", "[text]") {
    REQUIRE(kMainThreadMarked);

    const TempTree tree;
    tree.write("strings/en.json", kEnglish);

    FileSystem filesystem;
    REQUIRE(filesystem.mount("/", tree.root(), 0).has_value());

    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());
    Catalog catalog;

    const auto path = VirtualPath::parse("strings/en.json");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::StringTable);
    REQUIRE(id.has_value());
    REQUIRE(settle(*registry, catalog, *id));
    REQUIRE(catalog.lookup("ui.cancel") == "Cancel");

    // Rewritten with a later timestamp, because the reload check compares modification times
    // and a file written twice in the same instant looks unchanged.
    tree.write("strings/en.json", kEdited);
    std::filesystem::last_write_time(tree.root() / "strings/en.json",
                                     std::filesystem::file_time_type::clock::now() +
                                         std::chrono::seconds{60});

    const auto reloaded = registry->reload_changed();
    REQUIRE(reloaded.size() == 1);
    REQUIRE(settle(*registry, catalog, *id));

    CHECK(catalog.lookup("ui.ok") == "Right then");

    // The key the edit deleted is gone, not remembered. Merging would leave hot reload as a way
    // of accumulating stale text rather than of seeing an edit.
    CHECK(catalog.size() == 1);
    CHECK(catalog.lookup("ui.cancel") == "ui.cancel");
}

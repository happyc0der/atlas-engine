// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/registry.hpp>
#include <atlas/core/assert.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using atlas::ErrorCode;
using atlas::assets::AssetId;
using atlas::assets::AssetState;
using atlas::assets::AssetType;
using atlas::assets::FileSystem;
using atlas::assets::Registry;
using atlas::assets::VirtualPath;

namespace {

/// Establish this thread as the main one.
///
/// In an application the platform does this when it starts. These tests use the registry
/// without a platform, so they stand in for it: the registry asserts main-thread affinity on
/// every call that touches shared state, and an unmarked process fails that assertion rather
/// than passing by accident.
const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

class TempTree {
  public:
    TempTree() {
        std::error_code error;
        m_root = std::filesystem::temp_directory_path(error) /
                 std::format("atlas-registry-{}", reinterpret_cast<std::uintptr_t>(this));
        std::filesystem::create_directories(m_root, error);
    }

    ~TempTree() {
        std::error_code error;
        std::filesystem::remove_all(m_root, error);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    void write(std::string_view relative, std::string_view contents) const {
        const auto path = m_root / relative;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream stream(path, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    /// A valid two-by-two PNG: red, green, blue and white pixels.
    ///
    /// Generated once and embedded rather than written by hand. A hand-assembled PNG with a
    /// wrong chunk length or checksum decodes as "chunk not known", which looks like a
    /// decoder problem and is not. Distinct pixels so that a decode can be checked for
    /// content as well as for size.
    void write_png(std::string_view relative) const {
        static constexpr std::array<unsigned char, 75> kPng{
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49,
            0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06,
            0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44,
            0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x0C, 0x81, 0x34,
            0x18, 0x00, 0x00, 0x49, 0xC8, 0x09, 0xF7, 0x03, 0xD9, 0x64, 0xF1, 0x00, 0x00,
            0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
        };
        const auto path = m_root / relative;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream stream(path, std::ios::binary);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): writing raw bytes.
        stream.write(reinterpret_cast<const char*>(kPng.data()),
                     static_cast<std::streamsize>(kPng.size()));
    }

  private:
    std::filesystem::path m_root;
};

[[nodiscard]] VirtualPath path_of(std::string_view text) {
    auto parsed = VirtualPath::parse(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// Pump until the asset settles, or give up.
///
/// A bounded wait rather than a sleep: loading is asynchronous, and how long a worker takes
/// is not something a test should assert about. Failing after a generous limit turns a hang
/// into a readable failure.
[[nodiscard]] bool pump_until_settled(Registry& registry, AssetId id,
                                      std::chrono::milliseconds limit = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        registry.pump();
        const auto state = registry.state(id);
        if (state == AssetState::Decoded || state == AssetState::Failed) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

}  // namespace

TEST_CASE("a registry needs at least one worker", "[assets][registry]") {
    const TempTree tree;
    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());

    const auto registry = Registry::create(filesystem, {.worker_count = 0});
    REQUIRE_FALSE(registry.has_value());
    CHECK(registry.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("an asset identifier follows the path and type", "[assets][registry]") {
    // Stable across runs and machines, because it is written into save files and cache
    // names. Two types at one path are two assets, not one that two importers disagree over.
    const auto texture = AssetId::from("textures/grass.png", AssetType::Texture);
    const auto same = AssetId::from("textures/grass.png", AssetType::Texture);
    const auto shader = AssetId::from("textures/grass.png", AssetType::Shader);
    const auto other = AssetId::from("textures/stone.png", AssetType::Texture);

    CHECK(texture == same);
    CHECK_FALSE(texture == shader);
    CHECK_FALSE(texture == other);
    CHECK(texture.valid());
    CHECK_FALSE(AssetId{}.valid());
    CHECK(texture.to_string().size() == 16);
}

TEST_CASE("a texture loads asynchronously and reaches Decoded", "[assets][registry]") {
    const TempTree tree;
    tree.write_png("textures/pixel.png");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("textures/pixel.png"), AssetType::Texture);
    REQUIRE(id.has_value());

    // Queued immediately: the request returns before the work is done, which is the point.
    CHECK(atlas::assets::is_in_progress(registry->state(*id)));

    REQUIRE(pump_until_settled(*registry, *id));

    // Decoded rather than Ready: the pixels exist, but only the main thread may turn them
    // into a graphics resource, and nothing here has.
    CHECK(registry->state(*id) == AssetState::Decoded);

    const auto pending = registry->pending_finalisation();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0] == *id);

    const auto pixels = registry->take_texture(*id);
    REQUIRE(pixels.has_value());
    CHECK(pixels->width == 2);
    CHECK(pixels->height == 2);
    CHECK(pixels->pixels.size() == 2 * 2 * 4);

    // The first pixel is red, so the decode produced content rather than a blank of the
    // right size, and the channels are in the order the engine expects.
    CHECK(static_cast<unsigned char>(pixels->pixels[0]) == 255);
    CHECK(static_cast<unsigned char>(pixels->pixels[1]) == 0);
    CHECK(static_cast<unsigned char>(pixels->pixels[2]) == 0);
    CHECK(static_cast<unsigned char>(pixels->pixels[3]) == 255);

    // Taking moves the data out, so a second finaliser cannot use it twice.
    CHECK_FALSE(registry->take_texture(*id).has_value());

    registry->mark_ready(*id);
    CHECK(registry->state(*id) == AssetState::Ready);

    const auto info = registry->info(*id);
    REQUIRE(info.has_value());
    CHECK(info->load_count == 1);
    CHECK(info->error.empty());
}

TEST_CASE("requesting the same asset twice does not load it twice", "[assets][registry]") {
    const TempTree tree;
    tree.write_png("a.png");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto first = registry->request(path_of("a.png"), AssetType::Texture);
    const auto second = registry->request(path_of("a.png"), AssetType::Texture);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
    CHECK(registry->stats().total == 1);
}

TEST_CASE("a missing file fails with a reason rather than stopping", "[assets][registry]") {
    // An engine that halts because one texture is absent is much harder to work on than one
    // that records the failure and carries on.
    const TempTree tree;
    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("nowhere.png"), AssetType::Texture);
    REQUIRE(id.has_value());
    REQUIRE(pump_until_settled(*registry, *id));

    CHECK(registry->state(*id) == AssetState::Failed);

    const auto info = registry->info(*id);
    REQUIRE(info.has_value());
    CHECK_FALSE(info->error.empty());
    CHECK(info->error.find("nowhere.png") != std::string::npos);
    CHECK(registry->stats().failed == 1);
}

TEST_CASE("a file that is not an image fails to decode", "[assets][registry]") {
    const TempTree tree;
    tree.write("broken.png", "this is not a PNG at all");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("broken.png"), AssetType::Texture);
    REQUIRE(id.has_value());
    REQUIRE(pump_until_settled(*registry, *id));

    CHECK(registry->state(*id) == AssetState::Failed);
    const auto info = registry->info(*id);
    REQUIRE(info.has_value());
    CHECK_FALSE(info->error.empty());
}

TEST_CASE("an empty file fails rather than decoding to nothing", "[assets][registry]") {
    const TempTree tree;
    tree.write("empty.png", "");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("empty.png"), AssetType::Texture);
    REQUIRE(id.has_value());
    REQUIRE(pump_until_settled(*registry, *id));
    CHECK(registry->state(*id) == AssetState::Failed);
}

TEST_CASE("an asset without a type is refused", "[assets][registry]") {
    const TempTree tree;
    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("a.png"), AssetType::Unknown);
    REQUIRE_FALSE(id.has_value());
    CHECK(id.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("many assets load concurrently", "[assets][registry]") {
    // Two workers and a queue, so the point is that nothing is lost or duplicated rather
    // than that it is fast.
    const TempTree tree;
    constexpr int kCount = 32;
    for (int i = 0; i < kCount; ++i) {
        tree.write_png(std::format("textures/image{}.png", i));
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {.worker_count = 4});
    REQUIRE(registry.has_value());

    std::vector<AssetId> ids;
    for (int i = 0; i < kCount; ++i) {
        const auto id =
            registry->request(path_of(std::format("textures/image{}.png", i)), AssetType::Texture);
        REQUIRE(id.has_value());
        ids.push_back(*id);
    }

    for (const auto id : ids) {
        REQUIRE(pump_until_settled(*registry, id));
        CHECK(registry->state(id) == AssetState::Decoded);
    }

    CHECK(registry->stats().total == kCount);
    CHECK(registry->stats().awaiting_finalisation == kCount);
}

TEST_CASE("finalisation can fail and is recorded", "[assets][registry]") {
    // What happens when the pixels decoded but creating the graphics resource did not.
    const TempTree tree;
    tree.write_png("a.png");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("a.png"), AssetType::Texture);
    REQUIRE(id.has_value());
    REQUIRE(pump_until_settled(*registry, *id));

    registry->mark_failed(*id, "the device refused the texture");

    CHECK(registry->state(*id) == AssetState::Failed);
    const auto info = registry->info(*id);
    REQUIRE(info.has_value());
    CHECK(info->error == "the device refused the texture");
}

TEST_CASE("a changed file is reloaded", "[assets][registry]") {
    const TempTree tree;
    tree.write_png("a.png");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("a.png"), AssetType::Texture);
    REQUIRE(id.has_value());
    REQUIRE(pump_until_settled(*registry, *id));
    (void)registry->take_texture(*id);
    registry->mark_ready(*id);

    // Nothing changed yet, so nothing reloads.
    CHECK(registry->reload_changed().empty());

    // Set the time forward rather than sleeping: what hot reload compares is the recorded
    // modification time, and a test that waits on a clock is slow and occasionally wrong.
    std::error_code error;
    const auto now = std::filesystem::last_write_time(tree.root() / "a.png", error);
    REQUIRE_FALSE(error);
    std::filesystem::last_write_time(tree.root() / "a.png", now + std::chrono::seconds{60}, error);
    REQUIRE_FALSE(error);

    const auto reloaded = registry->reload_changed();
    REQUIRE(reloaded.size() == 1);
    CHECK(reloaded[0] == *id);

    REQUIRE(pump_until_settled(*registry, *id));
    CHECK(registry->state(*id) == AssetState::Decoded);

    registry->mark_ready(*id);
    const auto info = registry->info(*id);
    REQUIRE(info.has_value());
    CHECK(info->load_count == 2);
}

TEST_CASE("an asset still loading is not re-queued", "[assets][registry]") {
    // Re-queueing something in flight would load it twice and race the two results.
    const TempTree tree;
    tree.write_png("a.png");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto id = registry->request(path_of("a.png"), AssetType::Texture);
    REQUIRE(id.has_value());

    // Queued but not pumped, so it has not settled.
    CHECK(registry->reload_changed().empty());
}

TEST_CASE("the listing is ordered by path", "[assets][registry]") {
    // So that a status panel does not reshuffle itself every frame.
    const TempTree tree;
    for (const auto* name : {"zebra.png", "apple.png", "mango.png"}) {
        tree.write_png(name);
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    for (const auto* name : {"zebra.png", "apple.png", "mango.png"}) {
        REQUIRE(registry->request(path_of(name), AssetType::Texture).has_value());
    }

    const auto all = registry->all();
    REQUIRE(all.size() == 3);
    CHECK(all[0].path.text() == "apple.png");
    CHECK(all[1].path.text() == "mango.png");
    CHECK(all[2].path.text() == "zebra.png");
}

TEST_CASE("a registry with work still in flight shuts down cleanly", "[assets][registry]") {
    // The destructor has to stop workers that may be mid-read. Under ThreadSanitizer this is
    // where a missing signal or a data race would show up.
    const TempTree tree;
    for (int i = 0; i < 64; ++i) {
        tree.write_png(std::format("image{}.png", i));
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());

    {
        auto registry = Registry::create(filesystem, {.worker_count = 4});
        REQUIRE(registry.has_value());
        for (int i = 0; i < 64; ++i) {
            REQUIRE(registry->request(path_of(std::format("image{}.png", i)), AssetType::Texture)
                        .has_value());
        }
        // Destroyed here, with work still queued.
    }
    SUCCEED("the registry shut down with work outstanding");
}

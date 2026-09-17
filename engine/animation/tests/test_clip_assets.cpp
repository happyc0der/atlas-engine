// SPDX-License-Identifier: GPL-3.0-or-later
// A clip from a file on disk, through the registry, into the cache, and played.
//
// The parser is tested on its own and so is the evaluator. Neither working alone implies this:
// the parser could produce a shape the cache converts wrongly, and the cache could convert
// correctly something the parser never emits. What this covers is the seam between them, and
// the one thing neither can show — that an asset which decodes is actually claimed, and so does
// not sit in the registry being reported as stalled.
#include <atlas/animation/advance.hpp>
#include <atlas/assets/filesystem.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/core/assert.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using atlas::animation::advance;
using atlas::animation::ClipCache;
using atlas::animation::LoopMode;
using atlas::assets::AssetState;
using atlas::assets::AssetType;
using atlas::assets::FileSystem;
using atlas::assets::Registry;
using atlas::assets::VirtualPath;
using atlas::scene::Animator;
using atlas::scene::LocalTransform;
using atlas::scene::Scene;
using atlas::scene::StableId;
using Catch::Approx;

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

/// A temporary directory that cleans up after itself.
class TempTree {
  public:
    TempTree() {
        m_root = std::filesystem::temp_directory_path() /
                 ("atlas-clip-" + std::to_string(next_scratch_id()));
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

constexpr std::string_view kSlide = R"({
  "format": "atlas-animation-clip",
  "version": 1,
  "name": "slide",
  "duration_ms": 1000,
  "transform": [{"time_ms": 0, "x": 0.0}, {"time_ms": 1000, "x": 10.0}]
})";

/// A clip with a deliberately non-square grid and a frame track.
///
/// Four by two, not two by two: a square grid survives being transposed, so it would pass a
/// conversion that swapped the axes. Mutation testing found exactly that gap.
constexpr std::string_view kFrames = R"({
  "format": "atlas-animation-clip",
  "version": 1,
  "name": "cycle",
  "duration_ms": 800,
  "grid": {"columns": 4, "rows": 2},
  "frames": [{"time_ms": 0, "cell": 0}, {"time_ms": 400, "cell": 5}]
})";

constexpr std::string_view kFasterSlide = R"({
  "format": "atlas-animation-clip",
  "version": 1,
  "name": "slide",
  "duration_ms": 1000,
  "transform": [{"time_ms": 0, "x": 0.0}, {"time_ms": 1000, "x": 100.0}]
})";

/// Pump and finalise until the asset settles, or give up. A bounded wait rather than a sleep:
/// how long a worker takes is not something a test should assert about.
[[nodiscard]] bool settle(Registry& registry, ClipCache& clips, atlas::assets::AssetId id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        registry.pump();
        clips.finalise_pending(registry);
        const auto state = registry.state(id);
        if (state == AssetState::Ready || state == AssetState::Failed) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

}  // namespace

TEST_CASE("a clip loads from a file and plays", "[animation][assets]") {
    const TempTree tree;
    tree.write("animation/slide.clip.json", kSlide);

    FileSystem filesystem;
    REQUIRE(filesystem.mount("assets", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto path = VirtualPath::parse("animation/slide.clip.json");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::AnimationClip);
    REQUIRE(id.has_value());

    ClipCache clips;
    REQUIRE(settle(*registry, clips, *id));

    // Ready, not merely decoded. This is the assertion that would fail if the finaliser did
    // not exist: the asset would decode, sit unclaimed, and be reported as stalled ten seconds
    // later by the check M12 added.
    CHECK(registry->state(*id) == AssetState::Ready);
    CHECK(registry->stats().stalled == 0);
    CHECK(clips.size() == 1);

    const auto* clip = clips.find(*id);
    REQUIRE(clip != nullptr);
    CHECK(clip->name == "slide");
    CHECK(clip->duration_ns == 1'000'000'000ULL);

    // And it actually animates something, which is the part neither the parser's tests nor the
    // evaluator's can show on their own.
    Scene scene;
    const StableId entity = scene.create("thing");
    scene.set_local_transform(entity, LocalTransform{});
    scene.set_animator(entity,
                       Animator{.clip = *id, .loop = static_cast<std::uint8_t>(LoopMode::Loop)});

    advance(scene, clips, 500'000'000ULL);
    const auto* pose = scene.animation_pose(entity);
    REQUIRE(pose != nullptr);
    CHECK(pose->position_offset.x == Approx(5.0F).margin(1e-3));
}

TEST_CASE("a clip that does not parse fails rather than stalling", "[animation][assets]") {
    const TempTree tree;
    tree.write("animation/broken.clip.json", "{not a clip at all");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("assets", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto path = VirtualPath::parse("animation/broken.clip.json");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::AnimationClip);
    REQUIRE(id.has_value());

    ClipCache clips;
    REQUIRE(settle(*registry, clips, *id));

    CHECK(registry->state(*id) == AssetState::Failed);
    CHECK(clips.size() == 0);

    // A failed clip is not a frozen entity: the animator leaves it at the identity pose and
    // counts it, so a broken file looks like no animation rather than like a stuck one.
    Scene scene;
    const StableId entity = scene.create("thing");
    scene.set_local_transform(entity, LocalTransform{});
    scene.set_animator(entity, Animator{.clip = *id});
    const auto report = advance(scene, clips, 500'000'000ULL);
    CHECK(report.missing_clips == 1);
}

TEST_CASE("editing a clip on disk changes what plays next", "[animation][assets]") {
    // The whole point of hot reload for a clip: an author tunes a timing and sees the result
    // without restarting. The clock is deliberately not reset, so playback continues from
    // where it had reached rather than jumping back to the beginning.
    const TempTree tree;
    tree.write("animation/slide.clip.json", kSlide);

    FileSystem filesystem;
    REQUIRE(filesystem.mount("assets", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto path = VirtualPath::parse("animation/slide.clip.json");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::AnimationClip);
    REQUIRE(id.has_value());

    ClipCache clips;
    REQUIRE(settle(*registry, clips, *id));

    Scene scene;
    const StableId entity = scene.create("thing");
    scene.set_local_transform(entity, LocalTransform{});
    scene.set_animator(entity,
                       Animator{.clip = *id, .loop = static_cast<std::uint8_t>(LoopMode::Loop)});
    advance(scene, clips, 500'000'000ULL);
    const std::uint64_t elapsed = scene.animation_pose(entity)->elapsed_ns;
    CHECK(scene.animation_pose(entity)->position_offset.x == Approx(5.0F).margin(1e-3));

    // Rewritten with a later timestamp, because the reload check compares modification times
    // and a file written twice in the same instant looks unchanged.
    tree.write("animation/slide.clip.json", kFasterSlide);
    std::filesystem::last_write_time(tree.root() / "animation/slide.clip.json",
                                     std::filesystem::file_time_type::clock::now() +
                                         std::chrono::seconds{60});

    const auto reloaded = registry->reload_changed();
    REQUIRE(reloaded.size() == 1);
    REQUIRE(settle(*registry, clips, *id));

    // One clip, not two: the entry was replaced rather than added beside.
    CHECK(clips.size() == 1);

    // And the clock carried on rather than restarting, so the new clip is sampled at the same
    // place along it.
    advance(scene, clips, 0);
    CHECK(scene.animation_pose(entity)->elapsed_ns == elapsed);
    CHECK(scene.animation_pose(entity)->position_offset.x == Approx(50.0F).margin(1e-2));
}

TEST_CASE("the finaliser leaves other asset types alone", "[animation][assets]") {
    // Several finalisers share one registry and each skips what the others own. This is what
    // makes adding an asset type possible without the registry learning about devices.
    const TempTree tree;
    tree.write("shaders/pass.spv", "bytes that no clip parser would accept");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("assets", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto path = VirtualPath::parse("shaders/pass.spv");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::Shader);
    REQUIRE(id.has_value());

    // Bounded rather than counted: fifty rapid pumps is not a wait, it is fifty chances for a
    // worker that has not started yet to have finished.
    ClipCache clips;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline &&
           registry->state(*id) != AssetState::Decoded) {
        registry->pump();
        CHECK(clips.finalise_pending(*registry) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    // Still waiting, because nothing finalises a shader — which is the state the registry's
    // own stall check exists to report, and is not this cache's business to resolve.
    CHECK(registry->state(*id) == AssetState::Decoded);
    CHECK(clips.size() == 0);

    // And it stays there however many times this cache is asked.
    for (int i = 0; i < 20; ++i) {
        registry->pump();
        CHECK(clips.finalise_pending(*registry) == 0);
    }
    CHECK(registry->state(*id) == AssetState::Decoded);
}

TEST_CASE("a frame clip keeps its grid the way round it was written", "[animation][assets]") {
    const TempTree tree;
    tree.write("animation/cycle.clip.json", kFrames);

    FileSystem filesystem;
    REQUIRE(filesystem.mount("assets", tree.root()).has_value());
    auto registry = Registry::create(filesystem, {});
    REQUIRE(registry.has_value());

    const auto path = VirtualPath::parse("animation/cycle.clip.json");
    REQUIRE(path.has_value());
    const auto id = registry->request(*path, AssetType::AnimationClip);
    REQUIRE(id.has_value());

    ClipCache clips;
    REQUIRE(settle(*registry, clips, *id));
    REQUIRE(registry->state(*id) == AssetState::Ready);

    const auto* clip = clips.find(*id);
    REQUIRE(clip != nullptr);
    CHECK(clip->grid.columns == 4);
    CHECK(clip->grid.rows == 2);

    // And through to what actually gets drawn. Cell five of a four-by-two grid is the second
    // column of the second row; of a transposed two-by-four it would be the first column of
    // the third, which is a different picture at the same moment.
    Scene scene;
    const StableId entity = scene.create("thing");
    scene.set_local_transform(entity, LocalTransform{});
    scene.set_animator(entity,
                       Animator{.clip = *id, .loop = static_cast<std::uint8_t>(LoopMode::Loop)});

    advance(scene, clips, 500'000'000ULL);
    const auto* pose = scene.animation_pose(entity);
    REQUIRE(pose != nullptr);
    REQUIRE(pose->frame_uv.has_value());
    CHECK(pose->frame_uv->position.x == Approx(0.25F));
    CHECK(pose->frame_uv->position.y == Approx(0.5F));
    CHECK(pose->frame_uv->size.x == Approx(0.25F));
    CHECK(pose->frame_uv->size.y == Approx(0.5F));
}

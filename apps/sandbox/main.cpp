// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sandbox: the acceptance harness for the engine, not a game.
//
// At M1 it opens a window, pumps events, runs a fixed-step loop, and exits cleanly. There
// is nothing to draw yet, so the "render" step only counts frames. It is the composition
// root: subsystems are constructed here in order and destroyed in reverse, which is what
// makes startup and shutdown ordering visible rather than implicit. A runtime module takes
// over this role in M5, when a second application needs the same composition.

#include <atlas/app/frame_counters.hpp>
#include <atlas/app/log_options.hpp>
#include <atlas/app/main_guard.hpp>
#include <atlas/app/ppm.hpp>
#include <atlas/app/run_bounds.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/audio/device.hpp>
#include <atlas/core/args.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/build_info.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/edit/command.hpp>
#include <atlas/edit/history.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/scene/scene.hpp>
#include <atlas/scene/serialization.hpp>
#include <atlas/simulation/tick_accumulator.hpp>
#include <atlas/text/catalog.hpp>
#include <atlas/text/substitute.hpp>
#include <atlas/tools/debug_ui.hpp>
#include <atlas/tools/text_keys.hpp>

#include "scene.hpp"
#include "scene_demo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace {

constexpr atlas::log::Category kApp = atlas::log::category::kApp;

/// How often to look for changed asset files when hot reload is on.
constexpr std::uint64_t kReloadIntervalNs = 1'000'000'000;  // 1 s

/// Graphics validation layers are worth their cost while the renderer is being written, and
/// are not worth it in a build meant to be fast.
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

struct Options {
    bool headless = false;
    std::uint64_t max_frames = 0;  ///< 0 means run until asked to quit.
    std::uint64_t max_ticks = 0;   ///< 0 means no tick limit.
    std::uint32_t ticks_per_second = 60;
    bool unbounded = false;
    std::string_view video_driver;
    std::string_view log_level = "info";
    std::string_view log_file;
    std::string_view shader_dir = "assets/cooked/shaders";
    std::string_view screenshot;
    std::string_view audio_driver;
    bool no_render = false;
    bool no_overlay = false;
    bool no_gamepad = false;
    bool no_audio = false;
    bool hot_reload = false;
    std::string_view cache_dir;
    std::uint32_t grid = 100;
    std::string_view assets_dir = "assets/source";
    bool scene_graph = false;
    std::string_view scene_file = "build/sandbox-scene.json";
    std::uint64_t inspect = 0;  ///< 0 means no entity is selected to begin with.
};

void print_usage() {
    std::puts(R"(Atlas sandbox — engine acceptance harness.

Usage: atlas_sandbox [options]

Options:
  --headless             Run with no window at all. Needs --frames or --ticks to stop.
  --video-driver NAME    Ask for a specific video driver. "dummy" gives a real window
                         pipeline with no display, which is how CI exercises this code.
  --frames N             Stop after N frames. Default: run until asked to quit.
  --ticks N              Stop after N simulation ticks.
  --tps N                Simulation ticks per second. Default: 60.
  --unbounded            Run the simulation as fast as it will go, ignoring real time.
  --log-level LEVEL      trace, debug, info, warning, error, or fatal. Default: info.
  --log-file PATH        Also append log records to PATH.
  --shader-dir PATH      Where the cooked shaders live. Default: assets/cooked/shaders.
  --screenshot PATH      Write the last rendered frame to PATH as a PPM image, then exit.
  --no-render            Open a window but create no graphics device.
  --grid N               Draw an N by N field of quads. Default: 100, so ten thousand.
  --scene                Draw a small scene graph instead of the quad field. The scene is
                         built, saved, loaded back, and drawn from the loaded copy.
  --scene-file PATH      Where --scene writes and reads its scene.
                         Default: build/sandbox-scene.json.
  --inspect ID           Start with the given entity selected in the scene panel.
  --no-overlay           Do not create the debug overlay.
  --no-gamepad           Do not enumerate gamepads. On by default when there is a window.
  --no-audio             Open no audio device. On by default when there is a window.
  --audio-driver NAME    SDL audio driver, e.g. dummy.
  --edit-check           Apply and undo edits to the demo scene headlessly, then exit.
  --scene-check          Check the demo scene's shape and composition headlessly, then exit.
  --anim-check           Play the demo scene's clips headlessly and check the poses, then exit.
  --text-check           Resolve every interface string against the table, then exit.
  --assets-dir PATH      Directory to mount as the asset root. Default: assets/source.
  --hot-reload           Re-read assets whose files change while running.
  --cache-dir PATH       Keep decoded assets here between runs, so a repeated import is a
                         read rather than a decode. Default: no cache.
  --version              Print build identity and exit.
  --help                 Print this message and exit.

In a window, Escape or the close button quits.)");
}

[[nodiscard]] atlas::Result<Options> read_options(const atlas::Args& args) {
    Options options;
    options.headless = args.has("headless");
    options.unbounded = args.has("unbounded");
    options.video_driver = args.value_or("video-driver", std::string_view{});
    options.log_level = args.value_or("log-level", std::string_view{"info"});
    options.log_file = args.value_or("log-file", std::string_view{});
    options.shader_dir = args.value_or("shader-dir", std::string_view{"assets/cooked/shaders"});
    options.screenshot = args.value_or("screenshot", std::string_view{});
    options.no_render = args.has("no-render");
    options.no_overlay = args.has("no-overlay");
    options.no_gamepad = args.has("no-gamepad");
    options.no_audio = args.has("no-audio");
    options.audio_driver = args.value_or("audio-driver", std::string_view{});
    options.hot_reload = args.has("hot-reload");
    options.cache_dir = args.value_or("cache-dir", std::string_view{});
    options.scene_graph = args.has("scene");
    options.scene_file = args.value_or("scene-file", std::string_view{"build/sandbox-scene.json"});
    options.inspect = args.value_or("inspect", std::uint64_t{0}).value_or(0);
    options.assets_dir = args.value_or("assets-dir", std::string_view{"assets/source"});

    const auto grid = args.value_or("grid", std::uint64_t{100});
    if (!grid) {
        return std::unexpected(grid.error());
    }
    if (*grid == 0 || *grid > 4000) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument,
                         std::format("--grid must be in [1, 4000], got {}", *grid)));
    }
    options.grid = static_cast<std::uint32_t>(*grid);

    const auto frames = args.value_or("frames", std::uint64_t{0});
    if (!frames) {
        return std::unexpected(frames.error());
    }
    options.max_frames = *frames;

    const auto ticks = args.value_or("ticks", std::uint64_t{0});
    if (!ticks) {
        return std::unexpected(ticks.error());
    }
    options.max_ticks = *ticks;

    const auto tps = args.value_or("tps", std::uint64_t{60});
    if (!tps) {
        return std::unexpected(tps.error());
    }
    if (*tps == 0 || *tps > atlas::sim::TickAccumulator::kMaxTicksPerSecond) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument,
                         std::format("--tps must be in [1, {}], got {}",
                                     atlas::sim::TickAccumulator::kMaxTicksPerSecond, *tps)));
    }
    options.ticks_per_second = static_cast<std::uint32_t>(*tps);

    if (const auto status = args.reject_unknown(); !status) {
        return std::unexpected(status.error());
    }

    // A headless run has no window and therefore no close button: without a limit it would
    // never stop. Saying so is better than inventing a stop condition that does not exist.
    if (auto bound = atlas::app::validate_headless_bound(options.headless, options.max_frames,
                                                         options.max_ticks);
        !bound) {
        return std::unexpected(std::move(bound).error());
    }

    return options;
}

/// One simulation tick.
///
/// Empty on purpose: M1 delivers the time model, not a simulation. The zone marker is here
/// so the shape of the loop is visible in a profile before there is anything in it.
void step_simulation(atlas::Tick tick) {
    ATLAS_ZONE_NAMED("simulation tick");
    (void)tick;
}

/// Apply three edits to the demonstration scene, undo them, and require the bytes to match.
///
/// Runs with no device, no window and no assets, so it exercises the edit path in continuous
/// integration on every platform rather than only where a graphics device exists. What it adds
/// over the unit tests is the scene the application actually ships, the flag, and the exit
/// code: a composition root that wired the history to the wrong scene would pass every unit
/// test and fail here.
[[nodiscard]] atlas::Status run_edit_check() {
    // Normally Platform::create does this, and this check never creates a platform. The scene
    // asserts main-thread affinity on every mutation, so without it the first create aborts.
    atlas::mark_main_thread();

    auto built = atlas::sandbox::SceneDemo::build_demo_scene();
    if (!built) {
        return std::unexpected(built.error());
    }
    atlas::scene::Scene scene = std::move(*built);

    auto before = atlas::scene::to_text(scene);
    if (!before) {
        return std::unexpected(before.error());
    }

    // A leaf with a sprite, so the destroy has components to restore and the reparent has
    // somewhere to go. Chosen from the scene rather than assumed, so this keeps working if the
    // demonstration scene changes shape.
    atlas::scene::StableId target = atlas::scene::StableId::None;
    atlas::scene::StableId other_root = atlas::scene::StableId::None;
    for (const auto& view : scene.entities()) {
        if (scene.sprite(view.id) != nullptr && view.parent != atlas::scene::StableId::None &&
            !atlas::scene::valid(target)) {
            target = view.id;
        }
    }
    for (const atlas::scene::StableId root : scene.roots()) {
        if (root != scene.parent(target)) {
            other_root = root;
        }
    }
    if (!atlas::scene::valid(target) || !atlas::scene::valid(other_root)) {
        return atlas::fail(atlas::ErrorCode::NotFound,
                           "the demonstration scene no longer has a parented sprite and a "
                           "second root, which this check needs");
    }

    atlas::edit::History history{scene};
    if (auto status = history.apply(std::make_unique<atlas::edit::SetLocalTransform>(
            target, atlas::scene::LocalTransform{.position = {.x = 41.0F, .y = -7.0F}}));
        !status) {
        return status;
    }
    if (auto status = history.apply(std::make_unique<atlas::edit::Reparent>(target, other_root));
        !status) {
        return status;
    }
    if (auto status = history.apply(std::make_unique<atlas::edit::Destroy>(target)); !status) {
        return status;
    }

    const std::size_t applied = history.undo_depth();
    while (history.can_undo()) {
        if (!history.undo()) {
            return atlas::fail(atlas::ErrorCode::Internal,
                               history.last_error().has_value()
                                   ? history.last_error()->message()
                                   : std::string{"undo failed without an error"});
        }
    }

    auto after = atlas::scene::to_text(scene);
    if (!after) {
        return std::unexpected(after.error());
    }
    if (*after != *before) {
        return atlas::fail(atlas::ErrorCode::Internal,
                           "the scene differs after undoing every edit");
    }

    std::printf("edit check: %zu commands applied and undone, scene identical\n", applied);
    return atlas::ok();
}

/// The demonstration scene's shape and composition, with no device.
///
/// The scene the sandbox shows has had no automated coverage of any kind: no test runs
/// `--scene`, `SceneDemo`'s own methods are unreachable without a graphics device, and there is
/// no image comparison anywhere in the repository. That was tolerable while the scene was a
/// fixed demonstration. It stops being tolerable in the milestone that replaces how it moves,
/// because there would be nothing to measure the replacement against.
///
/// So this pins what must stay true across that change, and nothing that must not. It asserts
/// the hierarchy the demonstration exists to show, and the one property that makes an editor
/// usable on it: **after an edit, recomposing makes the drawn position agree with the authored
/// one.** It deliberately says nothing about how anything moves.
[[nodiscard]] atlas::Status run_scene_check() {
    atlas::mark_main_thread();

    auto built = atlas::sandbox::SceneDemo::build_demo_scene();
    if (!built) {
        return std::unexpected(built.error());
    }
    atlas::scene::Scene scene = std::move(*built);

    // A parent with children, at least one of which has a child of its own. That is the whole
    // point of the demonstration: relative transforms composing down a tree.
    atlas::scene::StableId parent = atlas::scene::StableId::None;
    for (const atlas::scene::StableId root : scene.roots()) {
        if (!scene.children(root).empty()) {
            parent = root;
        }
    }
    if (!atlas::scene::valid(parent)) {
        return atlas::fail(atlas::ErrorCode::NotFound,
                           "the demonstration scene has no root with children");
    }
    const auto children = scene.children(parent);
    atlas::scene::StableId grandchild = atlas::scene::StableId::None;
    for (const atlas::scene::StableId child : children) {
        if (!scene.children(child).empty()) {
            grandchild = scene.children(child).front();
        }
    }
    if (!atlas::scene::valid(grandchild)) {
        return atlas::fail(atlas::ErrorCode::NotFound,
                           "the demonstration scene has no third level, so composition past one "
                           "step is not being shown at all");
    }

    // Where the grandchild is drawn before anything moves.
    scene.update_transforms();
    const auto* composed = scene.world_transform(grandchild);
    if (composed == nullptr) {
        return atlas::fail(atlas::ErrorCode::Internal, "the grandchild has no world transform");
    }
    const float before_x = composed->matrix.at(0, 3);
    const float before_y = composed->matrix.at(1, 3);

    // Move the top of the tree through the history, exactly as the inspector does.
    constexpr float kShiftX = 17.0F;
    constexpr float kShiftY = -5.0F;
    const auto* local = scene.local_transform(parent);
    if (local == nullptr) {
        return atlas::fail(atlas::ErrorCode::Internal, "the parent has no local transform");
    }
    atlas::scene::LocalTransform moved = *local;
    moved.position.x += kShiftX;
    moved.position.y += kShiftY;

    atlas::edit::History history{scene};
    if (auto status =
            history.apply(std::make_unique<atlas::edit::SetLocalTransform>(parent, moved));
        !status) {
        return status;
    }

    // The history does not recompose — it bumps a revision and leaves that to whoever is
    // watching. This is the step the application itself was missing while paused, which is why
    // an edit changed the number and not the picture.
    scene.update_transforms();
    composed = scene.world_transform(grandchild);
    if (composed == nullptr) {
        return atlas::fail(atlas::ErrorCode::Internal,
                           "the grandchild lost its world transform after an edit");
    }

    // Two levels down, so this fails if composition stops at the first step.
    constexpr float kTolerance = 1e-3F;
    const float moved_x = composed->matrix.at(0, 3) - before_x;
    const float moved_y = composed->matrix.at(1, 3) - before_y;
    if (std::abs(moved_x - kShiftX) > kTolerance || std::abs(moved_y - kShiftY) > kTolerance) {
        return atlas::fail(
            atlas::ErrorCode::Internal,
            std::format("moving the root by ({}, {}) moved its grandchild by ({}, {})", kShiftX,
                        kShiftY, moved_x, moved_y));
    }

    // And undoing puts it back, which is the property the inspector's undo button promises.
    if (!history.undo()) {
        return atlas::fail(atlas::ErrorCode::Internal, "undo failed");
    }
    scene.update_transforms();
    composed = scene.world_transform(grandchild);
    if (composed == nullptr || std::abs(composed->matrix.at(0, 3) - before_x) > kTolerance ||
        std::abs(composed->matrix.at(1, 3) - before_y) > kTolerance) {
        return atlas::fail(atlas::ErrorCode::Internal,
                           "undoing the move did not put the grandchild back");
    }

    std::printf("scene check: %zu entities, composition reaches depth 3, edits recompose\n",
                scene.size());
    return atlas::ok();
}

/// The demonstration scene's animation, with no device: the whole path a frame takes.
///
/// This is M13's acceptance check, and what it asserts is chosen against what could plausibly
/// be wrong rather than against what is convenient to measure.
///
/// **Through the real asset pipeline**, not clips built in code. The clip files are committed
/// and hand-written, and a check that assembled its own clips would prove the evaluator works
/// while saying nothing about whether the files the application ships actually parse, resolve
/// to the identifiers the scene records, and play. Those are three ways this could be broken
/// that no unit test would notice.
///
/// **Poses to a tolerance, not to bytes.** The lab's determinism checks compare hashes exactly
/// because the lab is integer throughout. This is not: a pose reaches these numbers through
/// float easing, and whether a compiler contracts a multiply and an add into one instruction
/// changes the last bits. Comparing bytes here would make a check that passes on one machine
/// and fails on another for no reason anybody could act on. The clock itself is integer and so
/// *is* compared exactly, because it can be.
/// Resolve every key the interface can ask for against the shipped table, and say what is
/// missing.
///
/// **This is what makes "everything the overlay shows goes through the table" a fact rather
/// than a claim.** A missing key is not an error at runtime — it renders as itself, which is
/// legible and deliberate — so nothing in an ordinary run would ever notice one. Without this,
/// the milestone's exit criterion would rest on having looked carefully.
///
/// It checks the keys rather than the call sites, which is the one weakness worth naming: a
/// key used somewhere and never added to `text_keys.hpp` escapes both this and the reader,
/// because it renders as itself and nothing complains. What stops that is the rule that a call
/// site names a constant and never a string, and a grep of the overlay for a literal at an
/// ImGui call, which finds only widget identifiers.
[[nodiscard]] atlas::Status run_text_check(std::string_view assets_dir) {
    atlas::mark_main_thread();

    atlas::assets::FileSystem filesystem;
    if (auto status = filesystem.mount("assets", std::filesystem::path{assets_dir}); !status) {
        return std::unexpected(std::move(status).error().context("mounting the asset root"));
    }

    const auto path = atlas::assets::VirtualPath::parse("strings/en.json");
    if (!path) {
        return std::unexpected(atlas::Error(path.error()));
    }
    auto bytes = filesystem.read(*path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().context("reading the string table"));
    }
    auto imported = atlas::assets::import_string_table(*bytes, path->text());
    if (!imported) {
        return std::unexpected(std::move(imported).error().context("parsing the string table"));
    }

    atlas::text::Catalog catalog;
    if (auto status = catalog.load(*imported); !status) {
        return std::unexpected(std::move(status).error().context("loading the string table"));
    }

    std::vector<std::string_view> missing;
    for (const std::string_view key : atlas::tools::keys::kAllKeys) {
        if (catalog.lookup(key) == key) {
            // A key whose value happens to equal the key would report here too. None does, and
            // one would be a table entry written by somebody who had misunderstood the format,
            // so saying so is right rather than a false positive to suppress.
            missing.push_back(key);
        }
    }

    for (const std::string_view key : missing) {
        std::printf("text check: no entry for '%s'\n", std::string{key}.c_str());
    }
    if (!missing.empty()) {
        return std::unexpected(atlas::Error(
            atlas::ErrorCode::MalformedData,
            std::format("{} of {} keys have no entry in the '{}' table", missing.size(),
                        atlas::tools::keys::kAllKeys.size(), catalog.locale())));
    }

    // The substituter, through the strings that actually carry arguments, so the check covers
    // the shipped patterns rather than only the ones a unit test invented.
    const std::array<std::string_view, 1> one{"7"};
    const std::string undo =
        atlas::text::substitute(catalog.lookup(atlas::tools::keys::kUndoWith), one);
    if (!undo.contains('7')) {
        return std::unexpected(atlas::Error(atlas::ErrorCode::MalformedData,
                                            "'ui.undo_with' does not use its argument"));
    }

    std::printf("text check: %zu keys, all resolved in '%s', %zu entries in the table\n",
                atlas::tools::keys::kAllKeys.size(), std::string{catalog.locale()}.c_str(),
                catalog.size());
    return atlas::ok();
}

[[nodiscard]] atlas::Status run_anim_check(std::string_view assets_dir) {
    atlas::mark_main_thread();

    atlas::assets::FileSystem filesystem;
    if (auto status = filesystem.mount("assets", std::filesystem::path{assets_dir}); !status) {
        return std::unexpected(std::move(status).error().context("mounting the asset root"));
    }
    auto registry = atlas::assets::Registry::create(filesystem, {});
    if (!registry) {
        return std::unexpected(std::move(registry).error());
    }

    const atlas::sandbox::DemoAssets ids;
    atlas::animation::ClipCache clips;

    const std::array<std::pair<std::string_view, atlas::assets::AssetId>, 2> wanted{{
        {atlas::sandbox::kDemoOrbitClipPath, ids.orbit_clip},
        {atlas::sandbox::kDemoCycleClipPath, ids.cycle_clip},
    }};
    for (const auto& [path, expected] : wanted) {
        auto parsed = atlas::assets::VirtualPath::parse(path);
        if (!parsed) {
            return std::unexpected(std::move(parsed).error());
        }
        auto id = registry->request(*parsed, atlas::assets::AssetType::AnimationClip);
        if (!id) {
            return std::unexpected(std::move(id).error());
        }
        // The identifier a scene component carries is a hash of the path and the type. If the
        // registry derived a different one the clip would load and the scene would still not
        // play it, which is a failure with no symptom worth the name.
        if (*id != expected) {
            return atlas::fail(
                atlas::ErrorCode::Internal,
                std::format("'{}' did not resolve to the identifier the scene records", path));
        }
    }

    // Bounded rather than open: a worker that never finishes should fail this check, not hang
    // it. Decoding two small documents takes a few milliseconds; a second is far past generous.
    constexpr int kMaxPumps = 1000;
    for (int pump = 0; pump < kMaxPumps; ++pump) {
        registry->pump();
        clips.finalise_pending(*registry);
        const auto progress = registry->stats();
        if (progress.ready + progress.failed == progress.total) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto asset_stats = registry->stats();
    if (asset_stats.ready != wanted.size() || asset_stats.failed != 0) {
        return atlas::fail(atlas::ErrorCode::Internal,
                           std::format("the clips did not load: {} ready, {} failed, {} total",
                                       asset_stats.ready, asset_stats.failed, asset_stats.total));
    }
    // There was a second assertion here, on `awaiting_finalisation`, for the clip that decodes
    // and is never claimed. It was removed rather than kept: an asset sitting in Decoded is an
    // asset that is not Ready, so the count above has already failed by the time this could,
    // and an assertion that cannot fail is decoration rather than coverage. The mutation that
    // found this — making the clip finaliser skip its own type — is reported as caught by the
    // line above, which is what actually caught it.

    auto built = atlas::sandbox::SceneDemo::build_demo_scene(ids);
    if (!built) {
        return std::unexpected(built.error());
    }
    atlas::scene::Scene scene = std::move(*built);

    // What the file says before anything has been animated. Nothing below may change it,
    // which is the invariant the whole two-writer design rests on.
    auto before = atlas::scene::to_text(scene);
    if (!before) {
        return std::unexpected(before.error());
    }

    // Found from the scene rather than assumed, so this keeps working if the demonstration
    // changes shape: the animated root, and an animated entity below it.
    atlas::scene::StableId root = atlas::scene::StableId::None;
    atlas::scene::StableId descendant = atlas::scene::StableId::None;
    for (const atlas::scene::StableId id : scene.animators()) {
        if (scene.parent(id) == atlas::scene::StableId::None) {
            root = id;
        } else {
            descendant = id;
        }
    }
    if (!atlas::scene::valid(root) || !atlas::scene::valid(descendant)) {
        return atlas::fail(atlas::ErrorCode::NotFound,
                           "the demonstration scene no longer has an animated root and an "
                           "animated descendant, which this check needs");
    }
    const atlas::scene::StableId middle = scene.parent(descendant);

    // One frame at sixty a second, as an exact number of nanoseconds. The same sequence every
    // run, which is what makes the poses below reproducible at all: the module reads no clock.
    constexpr std::uint64_t kStepNs = 16'666'667;

    // What the clips produce, recorded rather than observed.
    //
    // Derived from the two committed clip files by working their own arithmetic out
    // separately, not by running this code and writing down what it said: a table produced by
    // the thing it checks is not a check. The orbit's keys are half a second apart with linear
    // easing, so a sample at a whole second is a key; the cycle is eight hundred milliseconds
    // long, so these four sample points land on all four of its cells, which is why these four
    // were chosen. The last is past the orbit's four-second duration and so also proves the
    // clip looped rather than held its last key.
    struct Expected {
        int steps;
        float position_x;
        float position_y;
        float rotation;
        float uv_x;
        float uv_y;
    };

    // The rotations are transcribed from the clip file digit for digit, and one of them is
    // close enough to pi that the linter offers to replace it with the constant. Declining is
    // the point: this table records what `orbit.clip.json` says, not what pi is. Swapping in a
    // more accurate constant would make the check pass against a file that no longer says this,
    // which is the one thing a recorded table must not do.
    constexpr std::array<Expected, 4> kExpected{{
        {.steps = 60,
         .position_x = 0.0F,
         .position_y = 18.0F,
         .rotation = 1.570796F,
         .uv_x = 0.5F,
         .uv_y = 0.0F},
        {.steps = 120,
         .position_x = -30.0F,
         .position_y = 0.0F,
         // NOLINTNEXTLINE(modernize-use-std-numbers)
         .rotation = 3.141593F,
         .uv_x = 0.0F,
         .uv_y = 0.5F},
        {.steps = 180,
         .position_x = 0.0F,
         .position_y = -18.0F,
         .rotation = 4.712389F,
         .uv_x = 0.5F,
         .uv_y = 0.5F},
        {.steps = 240,
         .position_x = 30.0F,
         .position_y = 0.0F,
         .rotation = 0.0F,
         .uv_x = 0.0F,
         .uv_y = 0.0F},
    }};

    // Wide enough to absorb a compiler contracting a multiply and an add, narrow enough that
    // every wrong answer available here — the wrong key, the wrong cell, a clip that held
    // instead of looping — is far outside it.
    constexpr float kTolerance = 1e-3F;
    constexpr float kTurn = 2.0F * std::numbers::pi_v<float>;

    int step = 0;
    for (const Expected& expected : kExpected) {
        for (; step < expected.steps; ++step) {
            atlas::animation::advance(scene, clips, kStepNs);
        }
        scene.update_transforms();

        const auto* pose = scene.animation_pose(root);
        if (pose == nullptr) {
            return atlas::fail(atlas::ErrorCode::Internal, "the animated root has no pose");
        }
        // The clock is integer and so is compared exactly. There is no float anywhere between
        // the step and this number, and allowing it to drift would be allowing the one thing
        // an integer clock exists to prevent.
        const std::uint64_t elapsed = static_cast<std::uint64_t>(expected.steps) * kStepNs;
        if (pose->elapsed_ns != elapsed) {
            return atlas::fail(atlas::ErrorCode::Internal,
                               std::format("after {} steps the clock reads {} ns, not {}",
                                           expected.steps, pose->elapsed_ns, elapsed));
        }
        if (std::abs(pose->position_offset.x - expected.position_x) > kTolerance ||
            std::abs(pose->position_offset.y - expected.position_y) > kTolerance ||
            std::abs(pose->rotation_offset - expected.rotation) > kTolerance) {
            return atlas::fail(
                atlas::ErrorCode::Internal,
                std::format("after {} steps the root's pose is ({:.4f}, {:.4f}) turned {:.4f}, "
                            "and the recorded table says ({:.4f}, {:.4f}) turned {:.4f}",
                            expected.steps, pose->position_offset.x, pose->position_offset.y,
                            pose->rotation_offset, expected.position_x, expected.position_y,
                            expected.rotation));
        }

        // The frame track, which is the other half of animation and the half that costs the
        // renderer nothing. Checked as the rectangle rather than as a cell index, because the
        // rectangle is what is drawn: a grid read as rows by columns instead of columns by
        // rows produces the right cell number and the wrong picture.
        const auto* frame = scene.animation_pose(descendant);
        if (frame == nullptr || !frame->frame_uv.has_value()) {
            return atlas::fail(atlas::ErrorCode::Internal,
                               "the animated descendant has no frame rectangle");
        }
        if (std::abs(frame->frame_uv->position.x - expected.uv_x) > kTolerance ||
            std::abs(frame->frame_uv->position.y - expected.uv_y) > kTolerance) {
            return atlas::fail(
                atlas::ErrorCode::Internal,
                std::format("after {} steps the frame is at ({:.3f}, {:.3f}) and the recorded "
                            "table says ({:.3f}, {:.3f})",
                            expected.steps, frame->frame_uv->position.x,
                            frame->frame_uv->position.y, expected.uv_x, expected.uv_y));
        }

        // The rotation has to survive composition to be worth anything: the root turns and
        // every descendant hangs off it. This pins the arithmetic the draw path uses to get a
        // rotation back out of a composed matrix, which is otherwise checked nowhere — the
        // batcher's own GPU test proves it draws a rotation it is handed, not that this is the
        // rotation it should have been handed.
        const auto* composed = scene.world_transform(middle);
        if (composed == nullptr) {
            return atlas::fail(atlas::ErrorCode::Internal, "a child has no world transform");
        }
        const float drawn = atlas::sandbox::decompose(composed->matrix).rotation;
        // Compared as an angle rather than as a number. The clip's rotation is deliberately
        // unwrapped — a key at two pi means a whole revolution — and a matrix cannot remember
        // how many turns it took to get where it is, so the two agree modulo a turn and this
        // is the comparison that says so.
        if (std::abs(std::remainder(drawn - expected.rotation, kTurn)) > kTolerance) {
            return atlas::fail(atlas::ErrorCode::Internal,
                               std::format("after {} steps a child is drawn turned {:.4f}, and "
                                           "its animated parent is turned {:.4f}",
                                           expected.steps, drawn, expected.rotation));
        }
    }

    // The byte test, at the level of the whole program. Two hundred and forty frames of
    // animation have been written into this scene and the file it would save is unchanged to
    // the byte, because the animator writes only the derived pose. That is what keeps the
    // sandbox's save-load-save check meaningful while clips are playing.
    auto after = atlas::scene::to_text(scene);
    if (!after) {
        return std::unexpected(after.error());
    }
    if (*after != *before) {
        return atlas::fail(atlas::ErrorCode::Internal,
                           "animating the scene changed what it would save");
    }

    // And the other half of the two-writer split: an edit lands while the clips are playing.
    // Before M13 this was impossible on this scene — the animation wrote the authored
    // transform, so an edit to an animated root survived until the next tick and no longer.
    const auto* authored = scene.local_transform(root);
    if (authored == nullptr) {
        return atlas::fail(atlas::ErrorCode::Internal, "the animated root has no local transform");
    }
    const atlas::scene::LocalTransform original = *authored;

    constexpr float kShiftX = 11.0F;
    constexpr float kShiftY = -6.0F;
    atlas::scene::LocalTransform dragged = original;
    dragged.position.x += kShiftX;
    dragged.position.y += kShiftY;

    atlas::edit::History history{scene};
    if (auto status =
            history.apply(std::make_unique<atlas::edit::SetLocalTransform>(root, dragged));
        !status) {
        return status;
    }
    // Another frame of animation on top of the edit, deliberately. If the animator wrote the
    // authored transform this is the step that would throw the edit away.
    atlas::animation::advance(scene, clips, kStepNs);
    scene.update_transforms();

    // Where the root is drawn must be its authored position plus its pose, and the pose is
    // read back rather than assumed still: the frame above moved the clip on as well.
    const auto check_placement = [&](const atlas::scene::LocalTransform& expected,
                                     std::string_view what) -> atlas::Status {
        const auto* world = scene.world_transform(root);
        const auto* pose = scene.animation_pose(root);
        if (world == nullptr || pose == nullptr) {
            return atlas::fail(atlas::ErrorCode::Internal, "the animated root lost a component");
        }
        const atlas::math::Vec2 drawn = atlas::sandbox::decompose(world->matrix).position;
        const float drift_x = drawn.x - (expected.position.x + pose->position_offset.x);
        const float drift_y = drawn.y - (expected.position.y + pose->position_offset.y);
        if (std::abs(drift_x) > kTolerance || std::abs(drift_y) > kTolerance) {
            return atlas::fail(
                atlas::ErrorCode::Internal,
                std::format("{}: the root is drawn at ({:.4f}, {:.4f}), which is not where its "
                            "authored position and its pose add up to",
                            what, drawn.x, drawn.y));
        }
        return atlas::ok();
    };
    if (auto status = check_placement(dragged, "after a drag while animating"); !status) {
        return status;
    }

    // Undo takes the drag back and leaves the animation entirely alone, which is the sentence
    // the whole of ADR-0012 exists to make true. The authored value is compared exactly: undo
    // restores a recorded transform rather than recomputing one, so anything but equality is
    // a defect and not a rounding difference.
    if (!history.undo()) {
        return atlas::fail(atlas::ErrorCode::Internal, "undoing the drag failed");
    }
    scene.update_transforms();
    const auto* restored = scene.local_transform(root);
    if (restored == nullptr || restored->position != original.position) {
        return atlas::fail(atlas::ErrorCode::Internal,
                           "undoing the drag did not restore the authored position");
    }
    if (auto status = check_placement(original, "after undoing the drag"); !status) {
        return status;
    }
    if (scene.animation_pose(root) == nullptr) {
        return atlas::fail(atlas::ErrorCode::Internal, "undo removed the animation");
    }

    std::printf("anim check: %d frames, 2 clips, poses match the recorded table, saved bytes "
                "unchanged, a drag while animating survives and undoes\n",
                step);
    return atlas::ok();
}

[[nodiscard]] atlas::Status run(int argc, const char* const* argv) {
    auto args = atlas::Args::parse(argc, argv);
    if (!args) {
        return std::unexpected(std::move(args).error());
    }

    if (args->has("help")) {
        print_usage();
        return atlas::ok();
    }
    if (args->has("version")) {
        std::printf("%s\n", std::string{atlas::build_info::summary()}.c_str());
        return atlas::ok();
    }

    if (args->has("edit-check")) {
        return run_edit_check();
    }

    if (args->has("scene-check")) {
        return run_scene_check();
    }

    if (args->has("anim-check")) {
        return run_anim_check(args->value_or("assets-dir", std::string_view{"assets/source"}));
    }

    if (args->has("text-check")) {
        return run_text_check(args->value_or("assets-dir", std::string_view{"assets/source"}));
    }

    const auto options = read_options(*args);
    if (!options) {
        return std::unexpected(options.error());
    }

    const atlas::app::LogSession logging;
    // Owned here, so it dies with this function and the sink registry's weak reference to
    // it simply stops resolving. Four thousand records is a few seconds of a busy frame loop
    // and about a megabyte, which is worth having when something goes wrong once.
    const auto log_buffer = std::make_shared<atlas::log::LogBuffer>(4096);

    if (const auto status =
            atlas::app::configure_logging(options->log_level, options->log_file, log_buffer);
        !status) {
        return status;
    }

    ATLAS_THREAD_NAME("main");
    ATLAS_LOG_INFO(kApp, "startup: {}", atlas::build_info::summary());

    atlas::assets::FileSystem filesystem;
    if (auto status = filesystem.mount("assets", std::filesystem::path{options->assets_dir});
        !status) {
        return std::unexpected(std::move(status).error().context("mounting the asset root"));
    }

    auto registry = atlas::assets::Registry::create(
        filesystem, {.cache_directory = std::filesystem::path{options->cache_dir}});
    if (!registry) {
        return std::unexpected(std::move(registry).error().context("starting the asset registry"));
    }

    // The interface's own text, requested like any other asset so that hot reload covers it.
    // A table that fails to load is not fatal: the overlay then shows its keys, which is what
    // a missing key does everywhere else and is legible rather than blank.
    atlas::text::Catalog catalog;
    if (const auto strings_path = atlas::assets::VirtualPath::parse("strings/en.json")) {
        if (const auto requested =
                registry->request(*strings_path, atlas::assets::AssetType::StringTable);
            !requested) {
            ATLAS_LOG_WARN(kApp, "no string table: {}", requested.error());
        }
    }

    // Subsystems are constructed in dependency order and destroyed in reverse, by scope.
    // The gamepad subsystem follows the window: a headless run has nothing to aim, and
    // enumerating input devices there is work with no consumer that can also raise a
    // permission prompt. So every headless test behaves exactly as it did before.
    auto platform = atlas::platform::Platform::create({
        .video = !options->headless,
        .video_driver = options->video_driver,
        .app_name = "Atlas sandbox",
        .gamepad = !options->headless && !options->no_gamepad,
        .audio = !options->headless && !options->no_audio,
        .audio_driver = options->audio_driver,
    });
    if (!platform) {
        return std::unexpected(std::move(platform).error().context("starting the platform"));
    }

    // Declared after the platform so it is destroyed before it: the platform's destructor
    // shuts down every window-system subsystem at once, including the one this device's stream
    // lives on. Note that the asset registry above is declared *earlier* and so outlives the
    // platform, which is fine for it and would not be for this.
    std::optional<atlas::audio::AudioDevice> audio;
    atlas::assets::AssetId ambient_id;
    atlas::audio::VoiceHandle ambient_voice;
    if (platform->has_audio_support()) {
        auto opened = atlas::audio::AudioDevice::create();
        if (!opened) {
            ATLAS_LOG_WARN(kApp, "no audio device ({}); continuing without sound", opened.error());
            audio = atlas::audio::AudioDevice::null();
        } else {
            audio = std::move(*opened);
        }

        // Requested, not awaited, exactly as the texture is. The identifier comes back now and
        // the samples arrive later; until they do there is silence, which is the right
        // behaviour for a sound and is why audio has no equivalent of the magenta fallback.
        if (auto path = atlas::assets::VirtualPath::parse("audio/ambient_loop.wav"); !path) {
            ATLAS_LOG_WARN(kApp, "the ambient loop's path is invalid: {}", path.error());
        } else if (auto requested = registry->request(*path, atlas::assets::AssetType::AudioClip);
                   !requested) {
            ATLAS_LOG_WARN(kApp, "the ambient loop could not be requested: {}", requested.error());
        } else {
            ambient_id = *requested;
        }
    }

    atlas::platform::Window window;
    if (!options->headless) {
        auto created = platform->create_window({.title = "Atlas sandbox"});
        if (!created) {
            return std::unexpected(std::move(created).error().context("opening a window"));
        }
        window = std::move(*created);
    }

    // The device and the renderer are optional: a headless run has neither, and a run with
    // --no-render deliberately skips them to exercise the window path on its own.
    std::optional<atlas::rhi::Device> device;
    std::optional<atlas::sandbox::DemoScene> scene;
    std::optional<atlas::sandbox::SceneDemo> scene_demo;
    std::optional<atlas::tools::DebugUi> overlay;

    if (!options->headless && !options->no_render && window.valid()) {
        auto created = atlas::rhi::Device::create({.debug = kDebugBuild}, window);
        if (!created) {
            return std::unexpected(
                std::move(created).error().context("creating the graphics device"));
        }
        device = std::move(*created);

        if (options->scene_graph) {
            auto demo = atlas::sandbox::SceneDemo::create(
                *device, *registry,
                {.shader_directory = options->shader_dir,
                 .save_path = std::filesystem::path{options->scene_file}});
            if (!demo) {
                return std::unexpected(std::move(demo).error().context("building the scene demo"));
            }
            scene_demo = std::move(*demo);
            scene_demo->resize(window.pixel_size().width, window.pixel_size().height);
        } else {
            auto demo =
                atlas::sandbox::DemoScene::create(*device, *registry,
                                                  {.grid_width = options->grid,
                                                   .grid_height = options->grid,
                                                   .shader_directory = options->shader_dir});
            if (!demo) {
                return std::unexpected(std::move(demo).error().context("building the demo scene"));
            }
            scene = std::move(*demo);
            scene->resize(window.pixel_size().width, window.pixel_size().height);
        }

        if (!options->no_overlay) {
            auto ui = atlas::tools::DebugUi::create(*device, window);
            if (ui) {
                ui->set_catalog(&catalog);
            }
            if (!ui) {
                // Not fatal: an engineering overlay that cannot start should not stop the
                // engine it is meant to observe.
                ATLAS_LOG_WARN(kApp, "the debug overlay is unavailable: {}", ui.error());
            } else {
                overlay = std::move(*ui);
                if (options->inspect != 0) {
                    overlay->select_entity(atlas::scene::StableId{options->inspect});
                }
            }
        }
    }

    auto accumulator = atlas::sim::TickAccumulator::create({
        .ticks_per_second = options->ticks_per_second,
    });
    if (!accumulator) {
        return std::unexpected(
            std::move(accumulator).error().context("configuring the tick scheduler"));
    }
    if (options->unbounded) {
        accumulator->set_speed(atlas::sim::Speed::unbounded());
    }

    ATLAS_LOG_INFO(kApp, "running: headless={} tps={} unbounded={} frames={} ticks={}",
                   options->headless, options->ticks_per_second, options->unbounded,
                   options->max_frames, options->max_ticks);

    atlas::SteadyClock clock;
    atlas::app::FrameCounters counters;
    std::uint64_t frame_index = 0;
    bool quit = false;
    bool captured = false;
    atlas::renderer::BatchStats last_batch;
    std::uint64_t reload_timer = 0;

    while (!quit) {
        ATLAS_ZONE_NAMED("frame");
        const std::uint64_t frame_ns = atlas::to_unsigned_ns(clock.tick());
        ++frame_index;

        const auto events = platform->pump();
        for (const auto& event : events) {
            // The overlay sees every event first, and reports whether it used one. A click
            // on a panel must not also pan the camera behind it.
            const bool consumed = overlay.has_value() && overlay->handle_event(event);
            if (consumed) {
                continue;
            }
            if (std::holds_alternative<atlas::platform::QuitRequested>(event) ||
                std::holds_alternative<atlas::platform::WindowCloseRequested>(event)) {
                quit = true;
            } else if (const auto* key = std::get_if<atlas::platform::KeyPressed>(&event)) {
                if (key->key == atlas::platform::Key::Escape) {
                    quit = true;
                } else if (key->key == atlas::platform::Key::Z && scene_demo.has_value() &&
                           (key->modifiers.control || key->modifiers.super)) {
                    // Already gated: the overlay saw this event first and, for a key, returns
                    // whether it wants the keyboard, so an event consumed by a future text
                    // field never reaches here and the shortcut cannot fight it.
                    auto& history = scene_demo->history();
                    const bool moved = key->modifiers.shift ? history.redo() : history.undo();
                    if (!moved && history.last_error().has_value()) {
                        ATLAS_LOG_WARN(kApp, "edit history discarded: {}", *history.last_error());
                    }
                }
            } else if (const auto* resized = std::get_if<atlas::platform::WindowResized>(&event)) {
                ATLAS_LOG_INFO(kApp, "window resized: logical={}x{} pixels={}x{}",
                               resized->size.width, resized->size.height, resized->pixel_size.width,
                               resized->pixel_size.height);
            } else if (std::holds_alternative<atlas::platform::WindowMinimized>(event)) {
                ATLAS_LOG_DEBUG(kApp, "window minimised");
            } else if (std::holds_alternative<atlas::platform::WindowRestored>(event)) {
                ATLAS_LOG_DEBUG(kApp, "window restored");
            }
        }

        // The pointer is ignored while the overlay owns it, so a drag on a panel does not
        // also move the world behind it. A gamepad is not gated that way: it has no pointer
        // to be over a panel with, so it keeps working while a panel has focus.
        const bool mouse_allowed = !(overlay.has_value() && overlay->wants_mouse());
        const float frame_seconds = static_cast<float>(frame_ns) / 1'000'000'000.0F;
        if (scene.has_value()) {
            scene->update(platform->input(), events, window.display_scale(), frame_seconds,
                          mouse_allowed);
        }
        if (scene_demo.has_value()) {
            scene_demo->update(platform->input(), events, window.display_scale(), frame_seconds,
                               mouse_allowed);
        }

        // Bring finished asset work in, then turn anything decoded into graphics resources.
        // Both are main-thread steps: workers produce bytes and stop there.
        registry->pump();
        std::size_t finalised = 0;
        if (scene.has_value()) {
            finalised = scene->finalise_assets(*registry);
        } else if (scene_demo.has_value()) {
            finalised = scene_demo->finalise_assets(*registry);
        }
        // A third finaliser over the same registry, filtering by type exactly as the other
        // two do. Hot reload therefore covers the interface's own text: editing a caption in
        // the table changes the panel on the next frame.
        finalised += catalog.finalise_pending(*registry);

        if (audio.has_value()) {
            // A second finaliser over the same registry, filtering by type. Both walk the same
            // sorted list and each skips what the other owns, which is the arrangement that
            // lets a new asset type arrive without the registry learning about devices.
            const std::size_t clips = audio->finalise_pending(*registry);
            finalised += clips;
            if (clips > 0 && ambient_id.valid()) {
                // Restart on the new samples. A looping voice started on the old clip would
                // play the old bytes forever, so a hot reload would appear to do nothing —
                // which is the exact failure a reload demonstration must not have.
                if (ambient_voice.valid()) {
                    (void)audio->stop(ambient_voice);
                }
                if (const auto clip = audio->clip_for(ambient_id); clip.valid()) {
                    ambient_voice = audio->play(
                        clip, {.volume = 0.6F, .loop = true, .bus = atlas::audio::Bus::Music});
                    ATLAS_LOG_INFO(kApp, "ambient loop playing");
                }
            }
        }
        if (finalised > 0) {
            ATLAS_LOG_INFO(kApp, "finalised {} asset(s)", finalised);
        }

        if (audio.has_value()) {
            audio->update();
        }

        // Polling rather than watching the filesystem: three platforms have three different
        // notification interfaces, and this is a development convenience. Once a second is
        // often enough to feel immediate and rare enough not to matter.
        if (options->hot_reload && frame_ns > 0) {
            reload_timer += frame_ns;
            if (reload_timer >= kReloadIntervalNs) {
                reload_timer = 0;
                const auto reloaded = registry->reload_changed();
                if (!reloaded.empty()) {
                    ATLAS_LOG_INFO(kApp, "{} asset(s) changed on disk", reloaded.size());
                }
            }
        }

        const auto tick_start = std::chrono::steady_clock::now();
        const auto plan = accumulator->advance(frame_ns);

        // The clamp lives in apps/common with its own test, because getting it wrong once
        // already cost a real defect.
        const std::uint32_t ticks_to_run = atlas::app::clamp_ticks(
            plan.ticks_to_run, accumulator->current_tick(), options->max_ticks);

        for (std::uint32_t i = 0; i < ticks_to_run; ++i) {
            const atlas::Tick tick = accumulator->current_tick() + i;
            step_simulation(tick);
        }
        accumulator->commit(ticks_to_run);
        const auto tick_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - tick_start)
                                           .count());

        counters.record(frame_ns, tick_ns, ticks_to_run, plan.dropped_ticks);

        // Animation is driven by frame time, not by ticks, and it sits outside the tick loop
        // on purpose. It is presentation: it reaches no simulation state, nothing it produces
        // is hashed, and a machine drawing at ninety frames a second must see the same motion
        // as one drawing at thirty. It writes only the derived pose, so it can run in the same
        // frame as an edit without either overwriting the other.
        if (scene_demo.has_value()) {
            scene_demo->advance(frame_ns);
        }

        // A minimised window has no image to draw into. The simulation carries on; only
        // presentation is skipped.
        if (device.has_value() && window.valid() && !window.is_minimized()) {
            ATLAS_ZONE_NAMED("present");

            auto frame = device->begin_frame();
            if (!frame) {
                // A failed frame is where device loss surfaces: every later call fails too,
                // so the first failure is the only one that names the cause. Atlas does not
                // recover, by charter; it reports the reason and stops with its own code.
                if (device->is_lost()) {
                    return atlas::fail(
                        atlas::ErrorCode::DeviceLost,
                        std::format("the graphics device was lost: {}", device->loss_reason()));
                }
                ATLAS_LOG_ERROR(kApp, "begin_frame failed: {}", frame.error());
                quit = true;
            } else {
                if (frame->has_swapchain_target()) {
                    // The last frame before exiting is the one worth capturing, so the
                    // request is made only once the loop is about to end.
                    const bool capture_now =
                        !options->screenshot.empty() && !captured &&
                        ((options->max_frames != 0 && frame_index >= options->max_frames) || quit);
                    if (capture_now) {
                        device->request_capture();
                        captured = true;
                    }

                    // The overlay is built and uploaded before the render pass opens.
                    // Uploading its vertex data begins a copy pass, and the graphics library
                    // refuses to nest one inside a render pass.
                    atlas::tools::DebugUi::PreparedFrame prepared_overlay{};
                    std::array<std::string, 8> overlay_values;
                    if (overlay.has_value()) {
                        const auto extent = frame->swapchain_extent();
                        overlay->begin_frame(static_cast<float>(frame_ns) / 1'000'000'000.0F,
                                             extent.width, extent.height);

                        overlay_values[0] =
                            std::format("{:.2f} ms", static_cast<double>(frame_ns) / 1e6);
                        overlay_values[1] = std::format("{}", accumulator->current_tick());
                        if (scene.has_value()) {
                            overlay_values[2] = std::format("{} of {}", scene->visible_last_frame(),
                                                            scene->quad_count());
                        } else if (scene_demo.has_value()) {
                            overlay_values[2] = std::format("{} of {}", last_batch.quads,
                                                            scene_demo->scene().size());
                        } else {
                            overlay_values[2] = "-";
                        }
                        overlay_values[3] = std::format("{}", last_batch.draw_calls);
                        overlay_values[4] = std::format("{} KiB", last_batch.bytes_uploaded / 1024);
                        if (scene.has_value()) {
                            overlay_values[5] = std::format("{:.2f}", scene->camera().zoom());
                        } else if (scene_demo.has_value()) {
                            overlay_values[5] = std::format("{:.2f}", scene_demo->camera().zoom());
                        } else {
                            overlay_values[5] = "-";
                        }

                        if (audio.has_value()) {
                            const auto audio_stats = audio->stats();
                            overlay_values[6] = std::format(
                                "{} voices, {} ms queued, {} underruns{}", audio_stats.voices,
                                audio_stats.queued_ms, audio_stats.underruns,
                                audio_stats.null_device ? ", no device" : "");
                        } else {
                            overlay_values[6] = "off";
                        }

                        const std::array<atlas::tools::Stat, 7> stats{{
                            {.label = atlas::tools::keys::kStatFrame, .value = overlay_values[0]},
                            {.label = atlas::tools::keys::kStatTick, .value = overlay_values[1]},
                            {.label = scene_demo.has_value()
                                          ? atlas::tools::keys::kStatSpritesDrawn
                                          : atlas::tools::keys::kStatQuadsVisible,
                             .value = overlay_values[2]},
                            {.label = atlas::tools::keys::kStatDrawCalls,
                             .value = overlay_values[3]},
                            {.label = atlas::tools::keys::kStatUploaded,
                             .value = overlay_values[4]},
                            {.label = atlas::tools::keys::kStatZoom, .value = overlay_values[5]},
                            {.label = atlas::tools::keys::kStatAudio, .value = overlay_values[6]},
                        }};
                        overlay->stats_panel(atlas::tools::keys::kTitleSandbox, stats);

                        // The panel takes the history, not the scene. The history exposes
                        // its scene as const and changes it only through undoable commands,
                        // so a widget still cannot reach past the validation Scene performs.
                        if (scene_demo.has_value()) {
                            overlay->scene_panel(atlas::tools::keys::kTitleScene,
                                                 scene_demo->history());
                        }

                        overlay->asset_panel(atlas::tools::keys::kTitleAssets, *registry);

                        if (overlay->log_console_panel(atlas::tools::keys::kTitleLog, *log_buffer)
                                .clear_requested) {
                            log_buffer->clear();
                        }

                        // Text input is switched on only while a field has focus, and off again
                        // when it loses focus. Leaving it on changes how the platform treats
                        // ordinary keys: with an input method engaged a shortcut key becomes a
                        // composition keystroke.
                        const bool want_text = overlay->wants_text_input();
                        if (want_text != window.text_input_active()) {
                            if (auto status = window.set_text_input_active(want_text); !status) {
                                ATLAS_LOG_WARN(kApp, "text input: {}", status.error());
                            }
                        }

                        // Where the input method should put its candidate list. The overlay
                        // reports in its own pixels; the window wants logical units, so the
                        // display scale converts. Without this the candidate list sits
                        // wherever it last was, usually over the text being typed.
                        if (want_text) {
                            const auto ime = overlay->ime_request();
                            const float scale = window.display_scale();
                            if (auto status = window.set_text_input_area(
                                    atlas::platform::Rect2D{.x = ime.x / scale,
                                                            .y = ime.y / scale,
                                                            .width = 1.0F,
                                                            .height = ime.line_height / scale},
                                    0.0F);
                                !status) {
                                ATLAS_LOG_WARN(kApp, "text input area: {}", status.error());
                            }
                        }

                        prepared_overlay = overlay->end_frame(*frame);
                    }

                    // Recompose here, every frame, whatever else happened.
                    //
                    // This is where an edit becomes visible. The history deliberately does not
                    // recompose — it bumps a revision and leaves that to whoever is watching —
                    // and until now nobody was: composition happened only at the tail of the
                    // demo's tick, which returns early while the animation is paused. So while
                    // paused, dragging an entity changed the authored number, changed what the
                    // inspector showed, and never moved the picture. The panel even displayed
                    // the contradiction, showing a local position that had moved beside a
                    // world translation that had not.
                    //
                    // After the overlay, because the panels apply this frame's edits, and
                    // before the pass, because the draw path reads the composed matrix.
                    if (scene_demo.has_value()) {
                        scene_demo->recompose();
                    }

                    auto pass = frame->begin_render_pass({
                        .colour = {.load = atlas::rhi::LoadOp::Clear,
                                   .clear_colour = {.r = 0.06F, .g = 0.07F, .b = 0.10F}},
                        .debug_name = "sandbox main pass",
                    });
                    if (!pass) {
                        ATLAS_LOG_ERROR(kApp, "begin_render_pass failed: {}", pass.error());
                    } else {
                        if (scene.has_value()) {
                            last_batch = scene->draw(*pass);
                        }
                        if (scene_demo.has_value()) {
                            last_batch = scene_demo->draw(*pass);
                        }
                        if (overlay.has_value()) {
                            overlay->draw(*pass, prepared_overlay);
                        }
                    }
                }

                if (const auto status = device->end_frame(std::move(*frame)); !status) {
                    ATLAS_LOG_ERROR(kApp, "end_frame failed: {}", status.error());
                    quit = true;
                }
            }
        }

        ATLAS_FRAME_MARK();

        // With no window there is no vsync and nothing to draw, so a realtime headless run
        // would spin the processor flat out producing millions of empty frames a second to
        // deliver sixty ticks. Waiting until the next tick is due costs nothing and is what
        // a headless server would do. Unbounded mode deliberately does not wait: throughput
        // is the entire point there.
        if (options->headless && !options->unbounded && ticks_to_run == 0) {
            std::this_thread::sleep_for(std::chrono::nanoseconds{
                atlas::app::headless_wait_ns(accumulator->tick_length_ns(), plan.alpha)});
        }

        if (options->max_frames != 0 && frame_index >= options->max_frames) {
            quit = true;
        }
        if (options->max_ticks != 0 && accumulator->current_tick() >= options->max_ticks) {
            quit = true;
        }
    }

    if (!options->screenshot.empty() && device.has_value()) {
        if (auto capture = device->take_capture()) {
            const std::filesystem::path path{options->screenshot};
            if (const auto status = atlas::app::write_ppm(path, *capture); !status) {
                ATLAS_LOG_ERROR(kApp, "writing the screenshot failed: {}", status.error());
            } else {
                ATLAS_LOG_INFO(kApp, "screenshot written to '{}' ({}x{})", path.string(),
                               capture->extent.width, capture->extent.height);
            }
        } else {
            ATLAS_LOG_WARN(kApp, "a screenshot was asked for but no frame was captured");
        }
    }

    {
        const auto asset_stats = registry->stats();
        ATLAS_LOG_INFO(kApp, "assets: {} total, {} ready, {} failed, cache hits={} misses={}",
                       asset_stats.total, asset_stats.ready, asset_stats.failed,
                       asset_stats.cache_hits, asset_stats.cache_misses);
        for (const auto& info : registry->all()) {
            if (info.state == atlas::assets::AssetState::Failed) {
                ATLAS_LOG_WARN(kApp, "  '{}' failed: {}", info.path.text(), info.error);
            }
        }
    }

    if (scene.has_value()) {
        ATLAS_LOG_INFO(kApp, "last frame: {} of {} quads visible, {} draw call(s), {} KiB uploaded",
                       scene->visible_last_frame(), scene->quad_count(), last_batch.draw_calls,
                       last_batch.bytes_uploaded / 1024);
    }

    // Wait for the device to finish before destroying anything it owns. The graphics library
    // defers destruction until work completes, so this is belt and braces today; it stops
    // being belt and braces the moment a backend is less forgiving, and the lab has always
    // done it. Two composition roots that shut down differently is one of them being wrong.
    if (device.has_value()) {
        if (const auto status = device->wait_idle(); !status) {
            ATLAS_LOG_WARN(kApp, "wait_idle at shutdown: {}", status.error());
        }
    }

    // Order matters and is explicit rather than left to scope: every one of these holds
    // graphics resources the device owns, and the device reports anything still live when it
    // shuts down. Forgetting one here is caught by that report, not by a crash.
    overlay.reset();
    scene.reset();
    scene_demo.reset();
    device.reset();

    ATLAS_LOG_INFO(kApp, "loop finished at tick {}", accumulator->current_tick());
    if (audio.has_value()) {
        const auto stats = audio->stats();
        ATLAS_LOG_INFO(kApp, "audio: {} voices peak, {} underruns, {} clips{}", stats.voices_peak,
                       stats.underruns, stats.clips, stats.null_device ? ", no device" : "");
    }
    counters.report();
    ATLAS_LOG_INFO(kApp, "shutdown");
    return atlas::ok();
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the signature of main is fixed by the standard.
int main(int argc, char** argv) {
    return atlas::app::guarded_main("atlas_sandbox", [&] { return run(argc, argv); });
}

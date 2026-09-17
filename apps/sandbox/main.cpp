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
#include <atlas/tools/debug_ui.hpp>

#include "scene.hpp"
#include "scene_demo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
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
    bool no_render = false;
    bool no_overlay = false;
    bool no_gamepad = false;
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
  --edit-check           Apply and undo edits to the demo scene headlessly, then exit.
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

    auto built = atlas::sandbox::SceneDemo::build_demo_scene(atlas::assets::AssetId{});
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

    // Subsystems are constructed in dependency order and destroyed in reverse, by scope.
    // The gamepad subsystem follows the window: a headless run has nothing to aim, and
    // enumerating input devices there is work with no consumer that can also raise a
    // permission prompt. So every headless test behaves exactly as it did before.
    auto platform = atlas::platform::Platform::create({
        .video = !options->headless,
        .video_driver = options->video_driver,
        .app_name = "Atlas sandbox",
        .gamepad = !options->headless && !options->no_gamepad,
    });
    if (!platform) {
        return std::unexpected(std::move(platform).error().context("starting the platform"));
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
                } else if (key->key == atlas::platform::Key::Space && scene_demo.has_value()) {
                    // The animation writes the sprite-bearing roots every tick, so an edit to
                    // one is overwritten within a frame. Pausing is what makes the editor
                    // usable on this scene at all.
                    scene_demo->set_animating(!scene_demo->animating());
                    ATLAS_LOG_INFO(kApp, "demo animation {}",
                                   scene_demo->animating() ? "running" : "paused");
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
        if (finalised > 0) {
            ATLAS_LOG_INFO(kApp, "finalised {} asset(s)", finalised);
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
            if (scene_demo.has_value()) {
                scene_demo->tick(tick, options->ticks_per_second);
            }
        }
        accumulator->commit(ticks_to_run);
        const auto tick_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - tick_start)
                                           .count());

        counters.record(frame_ns, tick_ns, ticks_to_run, plan.dropped_ticks);

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

                        const std::array<atlas::tools::Stat, 6> stats{{
                            {.label = "frame", .value = overlay_values[0]},
                            {.label = "tick", .value = overlay_values[1]},
                            {.label = scene_demo.has_value() ? "sprites drawn" : "quads visible",
                             .value = overlay_values[2]},
                            {.label = "draw calls", .value = overlay_values[3]},
                            {.label = "uploaded", .value = overlay_values[4]},
                            {.label = "zoom", .value = overlay_values[5]},
                        }};
                        overlay->stats_panel("Atlas", stats);

                        // The panel takes the history, not the scene. The history exposes
                        // its scene as const and changes it only through undoable commands,
                        // so a widget still cannot reach past the validation Scene performs.
                        if (scene_demo.has_value()) {
                            overlay->scene_panel("Scene", scene_demo->history());
                        }

                        overlay->asset_panel("Assets", *registry);

                        if (overlay->log_console_panel("Log", *log_buffer).clear_requested) {
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
    counters.report();
    ATLAS_LOG_INFO(kApp, "shutdown");
    return atlas::ok();
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the signature of main is fixed by the standard.
int main(int argc, char** argv) {
    return atlas::app::guarded_main("atlas_sandbox", [&] { return run(argc, argv); });
}

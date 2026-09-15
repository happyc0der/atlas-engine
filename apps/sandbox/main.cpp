// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sandbox: the acceptance harness for the engine, not a game.
//
// At M1 it opens a window, pumps events, runs a fixed-step loop, and exits cleanly. There
// is nothing to draw yet, so the "render" step only counts frames. It is the composition
// root: subsystems are constructed here in order and destroyed in reverse, which is what
// makes startup and shutdown ordering visible rather than implicit. A runtime module takes
// over this role in M5, when a second application needs the same composition.

#include <atlas/core/args.hpp>
#include <atlas/core/build_info.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/simulation/tick_accumulator.hpp>

#include "renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

constexpr atlas::log::Category kApp = atlas::log::category::kApp;

/// Upper bound on a single headless wait, so that a very low tick rate still checks its
/// stop conditions promptly.
constexpr std::uint64_t kMaxHeadlessSleepNs = 5'000'000;  // 5 ms

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
};

[[nodiscard]] atlas::Result<atlas::log::Severity> parse_severity(std::string_view name) {
    using atlas::log::Severity;
    if (name == "trace") {
        return Severity::Trace;
    }
    if (name == "debug") {
        return Severity::Debug;
    }
    if (name == "info") {
        return Severity::Info;
    }
    if (name == "warning" || name == "warn") {
        return Severity::Warning;
    }
    if (name == "error") {
        return Severity::Error;
    }
    if (name == "fatal") {
        return Severity::Fatal;
    }

    return std::unexpected(atlas::Error(
        atlas::ErrorCode::InvalidArgument,
        std::format("unknown log level '{}'; expected one of: trace, debug, info, warning, "
                    "error, fatal",
                    name)));
}

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
  --version              Print build identity and exit.
  --help                 Print this message and exit.

In a window, Escape or the close button quits.)");
}

/// Tears down the process-wide logging configuration when the run ends.
class LogSession {
  public:
    LogSession() = default;
    LogSession(const LogSession&) = delete;
    LogSession& operator=(const LogSession&) = delete;
    LogSession(LogSession&&) = delete;
    LogSession& operator=(LogSession&&) = delete;

    ~LogSession() { atlas::log::shutdown(); }
};

[[nodiscard]] atlas::Status configure_logging(const Options& options) {
    const auto severity = parse_severity(options.log_level);
    if (!severity) {
        return std::unexpected(severity.error());
    }
    atlas::log::set_min_severity(*severity);
    atlas::log::add_sink(atlas::log::make_console_sink());

    if (!options.log_file.empty()) {
        auto sink = atlas::log::make_file_sink(std::string{options.log_file});
        if (!sink) {
            return std::unexpected(std::move(sink).error().context("configuring logging"));
        }
        atlas::log::add_sink(std::move(*sink));
    }
    return atlas::ok();
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
    if (options.headless && options.max_frames == 0 && options.max_ticks == 0) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument,
                         "a headless run has no way to be asked to quit; pass --frames or "
                         "--ticks to bound it"));
    }

    return options;
}

/// Rolling timing counters.
///
/// Median and tails rather than a mean: a mean hides exactly the stalls that make an
/// application feel bad. See docs/PERFORMANCE.md.
///
/// The samples are a fixed-size ring. A headless run can produce millions of frames a
/// second, and keeping every one of them would grow without bound while answering a
/// question nobody asked: what matters is recent behaviour, not the whole history. Totals
/// are counted separately and are exact.
class FrameCounters {
  public:
    /// Enough samples for stable tail estimates, small enough to stay in cache.
    static constexpr std::size_t kWindow = 4096;

    FrameCounters() { m_frame_times.resize(kWindow, 0); }

    void record(std::uint64_t frame_ns, std::uint64_t tick_ns, std::uint32_t ticks,
                std::uint32_t dropped) noexcept {
        m_frame_times[m_next] = frame_ns;
        m_next = (m_next + 1) % kWindow;
        m_samples = std::min(m_samples + 1, kWindow);

        m_total_tick_ns += tick_ns;
        m_total_ticks += ticks;
        m_dropped_ticks += dropped;
        ++m_frames;
    }

    void report() const {
        if (m_frames == 0) {
            ATLAS_LOG_INFO(kApp, "no frames ran");
            return;
        }

        ATLAS_LOG_INFO(kApp, "frames={} ticks={} dropped={} tick time total us={}", m_frames,
                       m_total_ticks, m_dropped_ticks, m_total_tick_ns / 1000);

        std::vector<std::uint64_t> samples(
            m_frame_times.begin(), m_frame_times.begin() + static_cast<std::ptrdiff_t>(m_samples));
        std::ranges::sort(samples);

        const auto at = [&samples](double quantile) {
            const auto index =
                static_cast<std::size_t>(static_cast<double>(samples.size() - 1) * quantile);
            return samples[index];
        };

        ATLAS_LOG_INFO(kApp,
                       "frame time ns over the last {} frames: median={} p90={} p99={} max={}",
                       samples.size(), at(0.50), at(0.90), at(0.99), samples.back());

        if (m_dropped_ticks > 0) {
            ATLAS_LOG_WARN(kApp,
                           "{} ticks were dropped: the simulation did not keep up with the "
                           "requested speed",
                           m_dropped_ticks);
        }
    }

  private:
    std::vector<std::uint64_t> m_frame_times;
    std::size_t m_next = 0;
    std::size_t m_samples = 0;
    std::uint64_t m_total_tick_ns = 0;
    std::uint64_t m_frames = 0;
    std::uint64_t m_total_ticks = 0;
    std::uint64_t m_dropped_ticks = 0;
};

/// Write a captured frame to a Portable Pixmap.
///
/// PPM because it needs no library and every image viewer reads it. The capture is whatever
/// the swapchain format is, which is usually blue-green-red-alpha rather than the
/// red-green-blue PPM wants, so the channels are reordered here.
[[nodiscard]] atlas::Status write_ppm(const std::filesystem::path& path,
                                      const atlas::rhi::Device::Capture& capture) {
    if (capture.pixels.empty() || capture.extent.width == 0 || capture.extent.height == 0) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument, "nothing was captured"));
    }

    const bool bgra = capture.format == atlas::rhi::TextureFormat::Bgra8Unorm ||
                      capture.format == atlas::rhi::TextureFormat::Bgra8UnormSrgb;

    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::PermissionDenied,
                         std::format("cannot open '{}' for writing", path.string())));
    }

    stream << "P6\n" << capture.extent.width << ' ' << capture.extent.height << "\n255\n";

    const std::size_t pixel_count =
        static_cast<std::size_t>(capture.extent.width) * capture.extent.height;
    std::string rows;
    rows.resize(pixel_count * 3);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        const auto* pixel = &capture.pixels[i * 4];
        const auto r = static_cast<unsigned char>(pixel[bgra ? 2 : 0]);
        const auto g = static_cast<unsigned char>(pixel[1]);
        const auto b = static_cast<unsigned char>(pixel[bgra ? 0 : 2]);
        rows[(i * 3) + 0] = static_cast<char>(r);
        rows[(i * 3) + 1] = static_cast<char>(g);
        rows[(i * 3) + 2] = static_cast<char>(b);
    }

    stream.write(rows.data(), static_cast<std::streamsize>(rows.size()));
    if (!stream) {
        return std::unexpected(atlas::Error(atlas::ErrorCode::Internal,
                                            std::format("writing '{}' failed", path.string())));
    }
    return atlas::ok();
}

/// One simulation tick.
///
/// Empty on purpose: M1 delivers the time model, not a simulation. The zone marker is here
/// so the shape of the loop is visible in a profile before there is anything in it.
void step_simulation(atlas::Tick tick) {
    ATLAS_ZONE_NAMED("simulation tick");
    (void)tick;
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

    const auto options = read_options(*args);
    if (!options) {
        return std::unexpected(options.error());
    }

    const LogSession logging;
    if (const auto status = configure_logging(*options); !status) {
        return status;
    }

    ATLAS_THREAD_NAME("main");
    ATLAS_LOG_INFO(kApp, "startup: {}", atlas::build_info::summary());

    // Subsystems are constructed in dependency order and destroyed in reverse, by scope.
    auto platform = atlas::platform::Platform::create({
        .video = !options->headless,
        .video_driver = options->video_driver,
        .app_name = "Atlas sandbox",
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
    std::optional<atlas::sandbox::TriangleRenderer> renderer;

    if (!options->headless && !options->no_render && window.valid()) {
        auto created = atlas::rhi::Device::create({.debug = kDebugBuild}, window);
        if (!created) {
            return std::unexpected(
                std::move(created).error().context("creating the graphics device"));
        }
        device = std::move(*created);

        auto triangle = atlas::sandbox::TriangleRenderer::create(*device, options->shader_dir);
        if (!triangle) {
            return std::unexpected(
                std::move(triangle).error().context("preparing the triangle renderer"));
        }
        renderer = std::move(*triangle);
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
    FrameCounters counters;
    std::uint64_t frame_index = 0;
    bool quit = false;
    bool captured = false;

    while (!quit) {
        ATLAS_ZONE_NAMED("frame");
        const std::uint64_t frame_ns = atlas::to_unsigned_ns(clock.tick());
        ++frame_index;

        for (const auto& event : platform->pump()) {
            if (std::holds_alternative<atlas::platform::QuitRequested>(event) ||
                std::holds_alternative<atlas::platform::WindowCloseRequested>(event)) {
                quit = true;
            } else if (const auto* key = std::get_if<atlas::platform::KeyPressed>(&event)) {
                if (key->key == atlas::platform::Key::Escape) {
                    quit = true;
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

        const auto tick_start = std::chrono::steady_clock::now();
        const auto plan = accumulator->advance(frame_ns);
        for (std::uint32_t i = 0; i < plan.ticks_to_run; ++i) {
            step_simulation(accumulator->current_tick() + i);
        }
        accumulator->commit(plan.ticks_to_run);
        const auto tick_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - tick_start)
                                           .count());

        counters.record(frame_ns, tick_ns, plan.ticks_to_run, plan.dropped_ticks);

        // A minimised window has no image to draw into. The simulation carries on; only
        // presentation is skipped.
        if (device.has_value() && window.valid() && !window.is_minimized()) {
            ATLAS_ZONE_NAMED("present");

            auto frame = device->begin_frame();
            if (!frame) {
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

                    auto pass = frame->begin_render_pass({
                        .colour = {.load = atlas::rhi::LoadOp::Clear,
                                   .clear_colour = {.r = 0.06F, .g = 0.07F, .b = 0.10F}},
                        .debug_name = "sandbox main pass",
                    });
                    if (!pass) {
                        ATLAS_LOG_ERROR(kApp, "begin_render_pass failed: {}", pass.error());
                    } else if (renderer.has_value()) {
                        renderer->draw(*pass);
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
        if (options->headless && !options->unbounded && plan.ticks_to_run == 0) {
            const auto remaining =
                accumulator->tick_length_ns() -
                static_cast<std::uint64_t>(static_cast<double>(accumulator->tick_length_ns()) *
                                           static_cast<double>(plan.alpha));
            std::this_thread::sleep_for(
                std::chrono::nanoseconds{std::min<std::uint64_t>(remaining, kMaxHeadlessSleepNs)});
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
            if (const auto status = write_ppm(path, *capture); !status) {
                ATLAS_LOG_ERROR(kApp, "writing the screenshot failed: {}", status.error());
            } else {
                ATLAS_LOG_INFO(kApp, "screenshot written to '{}' ({}x{})", path.string(),
                               capture->extent.width, capture->extent.height);
            }
        } else {
            ATLAS_LOG_WARN(kApp, "a screenshot was asked for but no frame was captured");
        }
    }

    // The renderer holds resources the device owns, so it must go first. Destroying them in
    // the wrong order is the kind of mistake that only shows up in the leak report.
    renderer.reset();
    device.reset();

    ATLAS_LOG_INFO(kApp, "loop finished at tick {}", accumulator->current_tick());
    counters.report();
    ATLAS_LOG_INFO(kApp, "shutdown");
    return atlas::ok();
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the signature of main is fixed by the standard.
int main(int argc, char** argv) {
    // The outermost boundary. ADR-0005 forbids exceptions crossing module or thread
    // boundaries and treats allocation failure as fatal; this catch exists so that such a
    // failure exits with a diagnostic rather than through std::terminate.
    try {
        const auto status = run(argc, argv);

        if (!status) {
            const std::string message = status.error().to_string();
            std::fprintf(stderr, "atlas_sandbox: %s\n", message.c_str());
            ATLAS_LOG_ERROR(kApp, "exiting with failure: {}", message);
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "atlas_sandbox: unhandled exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "atlas_sandbox: unhandled exception of unknown type\n");
        return 1;
    }
}

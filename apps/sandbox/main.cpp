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
#include <atlas/simulation/tick_accumulator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
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

struct Options {
    bool headless = false;
    std::uint64_t max_frames = 0;  ///< 0 means run until asked to quit.
    std::uint64_t max_ticks = 0;   ///< 0 means no tick limit.
    std::uint32_t ticks_per_second = 60;
    bool unbounded = false;
    std::string_view video_driver;
    std::string_view log_level = "info";
    std::string_view log_file;
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
    std::uint64_t frame = 0;
    bool quit = false;

    while (!quit) {
        ATLAS_ZONE_NAMED("frame");
        const std::uint64_t frame_ns = atlas::to_unsigned_ns(clock.tick());
        ++frame;

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

        // There is no renderer yet. When there is, this is where a minimised window skips
        // presentation while the simulation carries on.
        if (!options->headless && window.valid() && !window.is_minimized()) {
            ATLAS_ZONE_NAMED("present");
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

        if (options->max_frames != 0 && frame >= options->max_frames) {
            quit = true;
        }
        if (options->max_ticks != 0 && accumulator->current_tick() >= options->max_ticks) {
            quit = true;
        }
    }

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

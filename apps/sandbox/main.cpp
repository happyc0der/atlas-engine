// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sandbox: the acceptance harness for the engine, not a game.
//
// At M0 it starts, logs, runs an empty loop, and exits cleanly. It is the composition
// root: subsystems are constructed here in order and destroyed in reverse, which is what
// makes startup and shutdown ordering visible rather than implicit. A `runtime` module
// takes over this role in M5, when a second application needs the same composition.

#include <atlas/core/args.hpp>
#include <atlas/core/build_info.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/core/result.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr atlas::log::Category kApp = atlas::log::category::kApp;

struct Options {
    bool headless = false;
    std::uint64_t iterations = 1;
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
  --headless             Run without a window. M0 has no window support, so every run is
                         headless whether or not this is given.
  --iterations N         Run exactly N loop iterations, then exit. Default: 1.
  --log-level LEVEL      trace, debug, info, warning, error, or fatal. Default: info.
  --log-file PATH        Also append log records to PATH.
  --version              Print build identity and exit.
  --help                 Print this message and exit.)");
}

/// Tears down the process-wide logging configuration when the run ends.
///
/// Sinks are removed and flushed by the destructor, so a failure anywhere after
/// construction still leaves the log intact and the registry empty.
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
    options.log_level = args.value_or("log-level", std::string_view{"info"});
    options.log_file = args.value_or("log-file", std::string_view{});

    const auto iterations = args.value_or("iterations", std::uint64_t{1});
    if (!iterations) {
        return std::unexpected(iterations.error());
    }
    options.iterations = *iterations;

    if (const auto status = args.reject_unknown(); !status) {
        return std::unexpected(status.error());
    }

    return options;
}

/// The main loop.
///
/// At M0 there is nothing to simulate and nothing to draw, so this exists to prove that the
/// loop, the frame markers, and the shutdown path work. It gains a real body in M1, when
/// there is an event source that can ask it to stop; until then the only stop condition is
/// the iteration count, which is why that has a default rather than meaning "forever".
void run_loop(const Options& options) {
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t iteration = 0;

    while (iteration < options.iterations) {
        ATLAS_ZONE_NAMED("frame");
        ++iteration;
        ATLAS_FRAME_MARK();
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    ATLAS_LOG_INFO(kApp, "loop finished: {} iterations in {} us", iteration, micros);
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
    ATLAS_LOG_INFO(kApp, "startup: iterations={} profiling={}", options->iterations,
                   atlas::build_info::profiling_enabled());

    // Say what is actually happening rather than echoing the flag: M0 has no window, so
    // the run is headless regardless of what was asked for.
    if (!options->headless) {
        ATLAS_LOG_INFO(kApp, "running headless: this build has no window support yet");
    }

    ATLAS_LOG_INFO(kApp, "running");
    run_loop(*options);

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
            // The log may not be configured yet when this fails, so report to stderr
            // directly as well. A failed run must never exit 0.
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

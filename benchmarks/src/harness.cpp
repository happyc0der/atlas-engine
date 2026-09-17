// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.hpp"

#include <atlas/core/build_info.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <thread>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace atlas::bench {
namespace {

#ifdef __APPLE__
[[nodiscard]] std::string sysctl_string(const char* name) {
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}
#endif

/// A duration in whichever unit shows it.
///
/// Fixed milliseconds would report a two-hundred-nanosecond operation as "0.000ms", which
/// tells the reader nothing and hides a regression completely.
[[nodiscard]] std::string format_duration(std::uint64_t nanoseconds) {
    if (nanoseconds >= 1'000'000) {
        return std::format("{:.3f}ms", static_cast<double>(nanoseconds) / 1'000'000.0);
    }
    if (nanoseconds >= 1'000) {
        return std::format("{:.3f}us", static_cast<double>(nanoseconds) / 1'000.0);
    }
    return std::format("{}ns", nanoseconds);
}

[[nodiscard]] std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(static_cast<double>(sorted.size() - 1) * fraction);
    return sorted[index];
}

}  // namespace

Environment describe_environment() {
    Environment environment;

#ifdef __APPLE__
    environment.cpu = sysctl_string("machdep.cpu.brand_string");
    environment.machine = sysctl_string("hw.model");
#endif
    if (environment.cpu.empty()) {
        environment.cpu = "unknown";
    }
    if (environment.machine.empty()) {
        environment.machine = "unknown";
    }

    // Hardware concurrency rather than a core count: it is what a thread pool would use, and
    // on a processor with both performance and efficiency cores the two differ.
    environment.cores = std::format("{} hardware threads", std::thread::hardware_concurrency());

    environment.os = std::string{build_info::system()};
    environment.compiler = std::string{build_info::compiler()};
    environment.build_type = std::string{build_info::build_type()};
    environment.commit = std::string{build_info::git_commit()};
    environment.dirty = build_info::git_dirty();
    environment.sanitizer = build_info::sanitizer() != "none";

    return environment;
}

Result measure(std::string_view name, std::string_view parameters, std::uint64_t iterations,
               std::uint64_t warmup, const std::function<void()>& body) {
    // Discarded, not measured: the first run of anything pays for cold caches, lazy page
    // faults and one-off allocations, which is startup cost rather than steady state.
    for (std::uint64_t i = 0; i < warmup; ++i) {
        body();
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);

    for (std::uint64_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
    }

    std::ranges::sort(samples);

    Result result;
    result.name = std::string{name};
    result.parameters = std::string{parameters};
    result.iterations = iterations;
    result.warmup_iterations = warmup;
    result.median_ns = percentile(samples, 0.50);
    result.p90_ns = percentile(samples, 0.90);
    result.p99_ns = percentile(samples, 0.99);
    result.min_ns = samples.empty() ? 0 : samples.front();
    result.max_ns = samples.empty() ? 0 : samples.back();
    return result;
}

Result measure_reported(std::string_view name, std::string_view parameters,
                        std::uint64_t iterations, std::uint64_t warmup,
                        const std::function<std::uint64_t()>& body) {
    for (std::uint64_t i = 0; i < warmup; ++i) {
        (void)body();
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const std::uint64_t elapsed = body();
        if (elapsed > 0) {
            samples.push_back(elapsed);
        }
    }

    std::ranges::sort(samples);

    Result result;
    result.name = std::string{name};
    result.parameters = std::string{parameters};
    result.iterations = samples.size();
    result.warmup_iterations = warmup;
    result.median_ns = percentile(samples, 0.50);
    result.p90_ns = percentile(samples, 0.90);
    result.p99_ns = percentile(samples, 0.99);
    result.min_ns = samples.empty() ? 0 : samples.front();
    result.max_ns = samples.empty() ? 0 : samples.back();
    return result;
}

std::vector<Benchmark>& registry() {
    static std::vector<Benchmark> instance;
    return instance;
}

bool register_benchmark(std::string_view name, std::function<std::vector<Result>()> run) {
    registry().push_back(Benchmark{.name = name, .run = std::move(run)});
    return true;
}

void write_json(const Environment& environment, const std::vector<Result>& results,
                std::string_view path) {
    std::ofstream stream{std::string{path}};
    if (!stream) {
        std::fprintf(stderr, "cannot open '%s' for writing\n", std::string{path}.c_str());
        return;
    }

    // Written by hand rather than with a library: Atlas has no JSON dependency, and the
    // shape here is fixed and small. tools/bench_baseline.py reads it.
    const auto quote = [](const std::string& text) { return std::format("\"{}\"", text); };

    stream << "{\n";
    stream << "  \"environment\": {\n";
    stream << "    \"machine\": " << quote(environment.machine) << ",\n";
    stream << "    \"cpu\": " << quote(environment.cpu) << ",\n";
    stream << "    \"cores\": " << quote(environment.cores) << ",\n";
    stream << "    \"os\": " << quote(environment.os) << ",\n";
    stream << "    \"compiler\": " << quote(environment.compiler) << ",\n";
    stream << "    \"build_type\": " << quote(environment.build_type) << ",\n";
    stream << "    \"commit\": " << quote(environment.commit) << ",\n";
    stream << "    \"dirty\": " << (environment.dirty ? "true" : "false") << ",\n";
    stream << "    \"sanitizer\": " << (environment.sanitizer ? "true" : "false") << "\n";
    stream << "  },\n";
    stream << "  \"results\": [\n";

    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        stream << "    {\n";
        stream << "      \"name\": " << quote(result.name) << ",\n";
        stream << "      \"parameters\": " << quote(result.parameters) << ",\n";
        stream << "      \"iterations\": " << result.iterations << ",\n";
        stream << "      \"warmup_iterations\": " << result.warmup_iterations << ",\n";
        stream << "      \"median_ns\": " << result.median_ns << ",\n";
        stream << "      \"p90_ns\": " << result.p90_ns << ",\n";
        stream << "      \"p99_ns\": " << result.p99_ns << ",\n";
        stream << "      \"min_ns\": " << result.min_ns << ",\n";
        stream << "      \"max_ns\": " << result.max_ns << ",\n";
        stream << "      \"units_per_iteration\": " << result.units_per_iteration << ",\n";
        stream << "      \"unit_name\": " << quote(result.unit_name) << ",\n";
        stream << "      \"metric\": "
               << quote(result.metric == Metric::Count ? "count" : "nanoseconds") << ",\n";
        stream << "      \"count_name\": " << quote(result.count_name) << "\n";
        stream << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }

    stream << "  ]\n";
    stream << "}\n";
}

void write_table(const Environment& environment, const std::vector<Result>& results) {
    std::printf("\n");
    std::printf("machine   : %s (%s, %s)\n", environment.machine.c_str(), environment.cpu.c_str(),
                environment.cores.c_str());
    std::printf("build     : %s %s, %s%s\n", environment.compiler.c_str(),
                environment.build_type.c_str(), environment.commit.c_str(),
                environment.dirty ? " (dirty)" : "");

    if (environment.build_type != "Release" && environment.build_type != "RelWithDebInfo") {
        std::printf("\nWARNING: this is a %s build. These numbers are not comparable with\n"
                    "         an optimised build and must not be recorded as a baseline.\n",
                    environment.build_type.c_str());
    }
    if (environment.sanitizer) {
        std::printf("\nWARNING: a sanitizer is enabled. These numbers measure the sanitizer.\n");
    }
    if (environment.dirty) {
        std::printf("\nWARNING: the working tree has uncommitted changes, so this result is\n"
                    "         not reproducible from the recorded commit.\n");
    }

    std::printf("\n%-34s %-18s %14s %12s %12s %14s\n", "benchmark", "parameters", "median", "p90",
                "p99", "throughput");
    std::printf("%s\n", std::string(108, '-').c_str());

    for (const auto& result : results) {
        if (result.metric == Metric::Count) {
            // A count is not a duration. Printing one through the millisecond format would
            // report eight allocations as "0.000ms", which is worse than saying nothing.
            const std::string suffix =
                result.count_name.empty() ? std::string{} : " " + result.count_name;
            std::printf("%-34s %-18s %14s %12s %12s %14s\n", result.name.c_str(),
                        result.parameters.c_str(),
                        std::format("{}{}", result.median_ns, suffix).c_str(), "-", "-", "-");
            continue;
        }

        std::string throughput = "-";
        if (result.units_per_iteration > 0 && result.median_ns > 0) {
            const double per_second = static_cast<double>(result.units_per_iteration) *
                                      1'000'000'000.0 / static_cast<double>(result.median_ns);
            throughput = std::format("{:.2f}M {}/s", per_second / 1'000'000.0, result.unit_name);
        }

        std::printf("%-34s %-18s %14s %12s %12s %14s\n", result.name.c_str(),
                    result.parameters.c_str(), format_duration(result.median_ns).c_str(),
                    format_duration(result.p90_ns).c_str(), format_duration(result.p99_ns).c_str(),
                    throughput.c_str());
    }
    std::printf("\n");
}

}  // namespace atlas::bench

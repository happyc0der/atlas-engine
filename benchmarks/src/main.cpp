// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the registered benchmarks and reports the results.
#include <atlas/core/args.hpp>
#include <atlas/core/log.hpp>

#include "harness.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        auto args = atlas::Args::parse(argc, argv);
        if (!args) {
            std::fprintf(stderr, "atlas_bench: %s\n", args.error().to_string().c_str());
            return 1;
        }

        if (args->has("help")) {
            std::puts(R"(Atlas benchmarks.

Usage: atlas_bench [options]

Options:
  --json PATH     Write machine-readable results to PATH.
  --filter NAME   Run only the benchmark group with this name.
  --quiet         Suppress engine log output, leaving only the results.
  --help          Print this message and exit.)");
            return 0;
        }

        const auto json_path = args->value_or("json", std::string_view{});
        const auto filter = args->value_or("filter", std::string_view{});
        const bool quiet = args->has("quiet");

        if (const auto status = args->reject_unknown(); !status) {
            std::fprintf(stderr, "atlas_bench: %s\n", status.error().to_string().c_str());
            return 1;
        }

        // Engine logging would interleave with the results, so it is quiet by default.
        atlas::log::set_min_severity(quiet ? atlas::log::Severity::Fatal
                                           : atlas::log::Severity::Warning);
        atlas::log::add_sink(atlas::log::make_console_sink());

        const auto environment = atlas::bench::describe_environment();

        std::vector<atlas::bench::Result> results;
        for (const auto& benchmark : atlas::bench::registry()) {
            if (!filter.empty() && benchmark.name != filter) {
                continue;
            }
            auto group = benchmark.run();
            results.insert(results.end(), std::make_move_iterator(group.begin()),
                           std::make_move_iterator(group.end()));
        }

        atlas::bench::write_table(environment, results);

        if (!json_path.empty()) {
            atlas::bench::write_json(environment, results, json_path);
            std::printf("wrote %s\n", std::string{json_path}.c_str());
        }

        atlas::log::shutdown();
        return results.empty() ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "atlas_bench: unhandled exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "atlas_bench: unhandled exception of unknown type\n");
        return 1;
    }
}

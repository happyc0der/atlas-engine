// SPDX-License-Identifier: GPL-3.0-or-later
//
// What looking a string up costs.
//
// Three scenarios:
//
//   text/lookup_hit     — the path that runs once per visible string per frame. A hash of a
//                         `string_view` and a bucket probe, with no allocation, which is what
//                         the transparent hasher exists to make true.
//   text/lookup_miss    — the same for a key with no entry. Deliberately measured, because a
//                         miss is the normal state of an interface somebody is still writing
//                         and it must not be the expensive one.
//   text/substitute     — one pattern with two arguments, which is the shape every composed
//                         string in the overlay has.
//
// **The overlay itself is not measured, and cannot be.** A benchmark that drew a panel would
// be measuring Dear ImGui and a graphics device. What is measured is the part this milestone
// added, which is the part that could be slow for a reason this project owns.
//
// ---------------------------------------------------------------------------------------
// PREDICTION, written before the first run and committed before the numbers exist.
//
// `text/lookup_hit`: **20 to 60 nanoseconds**. One `std::hash` over a short string, one bucket
// probe, one string comparison to confirm the key. No allocation. The floor is the timer's own
// resolution, which M15 measured at about 42 ns on this machine — so if this lands at the
// floor, the honest report is "below what this harness can resolve" rather than a number.
//
// `text/lookup_miss`: **within 2x of a hit** on the second and later occurrences of the same
// key. The first occurrence allocates a string to remember the key, and the harness's warm-up
// absorbs that; if this comes out far above a hit, the `contains`-before-`emplace` guard is not
// working and every missed frame is allocating.
//
// `text/substitute`: **100 to 300 ns** for a two-argument pattern. One `reserve`, one pass over
// perhaps thirty characters, and two appends. The single allocation is the `std::string` that
// comes back, and it is unavoidable: the function returns owned text.
//
// **The claim this is expected to support is that routing the interface through a table costs
// nothing worth reclaiming.** About 140 strings a frame at 60 ns is under 10 µs, which is
// 0.05% of a 16.6 ms frame, and most of those are looked up once and formatted anyway. Above
// **500 ns** for `lookup_hit` the cause will be an allocation per call, which would mean the
// transparent hasher is not being used and every lookup is building a `std::string` — the
// single mistake this design exists to avoid, and the reason the scenario is here at all.
//
// Being wrong here is fine and is why it is recorded first. Being unable to be wrong is not.
// ---------------------------------------------------------------------------------------

#include <atlas/text/catalog.hpp>
#include <atlas/text/substitute.hpp>
#include <atlas/tools/text_keys.hpp>

#include "harness.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using atlas::bench::Result;

[[noreturn]] void die(std::string_view what) {
    std::fprintf(stderr, "bench_text: %s\n", std::string{what}.c_str());
    std::abort();
}

std::vector<Result> run() {
    std::vector<Result> results;

    // Filled with the real keys rather than generated ones, so the hashing measures the string
    // lengths the interface actually has rather than a length chosen to flatter it.
    atlas::text::Catalog catalog;
    for (const std::string_view key : atlas::tools::keys::kAllKeys) {
        if (!catalog.insert(key, "a caption of about the usual length")) {
            die("filling the catalog");
        }
    }

    {
        std::size_t index = 0;
        results.push_back(atlas::bench::measure(
            "text/lookup_hit", std::format("keys={}", catalog.size()), 200'000, 20'000, [&] {
                // Rotating through every key rather than asking for one repeatedly, so this
                // measures a hash table rather than a single hot bucket.
                const std::string_view key =
                    atlas::tools::keys::kAllKeys[index++ % atlas::tools::keys::kAllKeys.size()];
                // The house pattern for keeping a result: `lookup` is a call into another
                // translation unit, so it cannot be elided, and the sink keeps what it returned
                // from being discarded as a value nobody wanted.
                const volatile std::size_t sink = catalog.lookup(key).size();
                (void)sink;
            }));
    }

    {
        results.push_back(atlas::bench::measure("text/lookup_miss", "keys=1", 200'000, 20'000, [&] {
            const volatile std::size_t sink = catalog.lookup("ui.not.in.the.table").size();
            (void)sink;
        }));
    }

    // Batched, and added after the first run rather than before it, which is worth saying
    // plainly. The three scenarios above all came out at the timer's own resolution — about
    // 42 ns on this machine, the same floor M15 measured — so they say "faster than this
    // harness can see" and nothing more. A batch of sixty-four divided back down resolves it.
    //
    // This is not the prediction being moved after the fact: the prediction stands as written
    // and is reported against as written in docs/PERFORMANCE.md. This is the measurement being
    // made capable of testing it, which the first version was not.
    {
        constexpr std::size_t kBatch = 64;
        std::size_t index = 0;
        results.push_back(atlas::bench::measure(
            "text/lookup_hit_x64", std::format("keys={}", catalog.size()), 20'000, 2'000, [&] {
                std::size_t total = 0;
                for (std::size_t i = 0; i < kBatch; ++i) {
                    const std::string_view key =
                        atlas::tools::keys::kAllKeys[index++ % atlas::tools::keys::kAllKeys.size()];
                    total += catalog.lookup(key).size();
                }
                const volatile std::size_t sink = total;
                (void)sink;
            }));
    }

    {
        const std::string_view pattern = "{0} shown, {1} hidden";
        const std::array<std::string_view, 2> args{"128", "12"};
        results.push_back(atlas::bench::measure("text/substitute", "args=2", 200'000, 20'000, [&] {
            const std::string out = atlas::text::substitute(pattern, args);
            const volatile std::size_t sink = out.size();
            (void)sink;
        }));

        constexpr std::size_t kBatch = 64;
        results.push_back(
            atlas::bench::measure("text/substitute_x64", "args=2", 20'000, 2'000, [&] {
                std::size_t total = 0;
                for (std::size_t i = 0; i < kBatch; ++i) {
                    total += atlas::text::substitute(pattern, args).size();
                }
                const volatile std::size_t sink = total;
                (void)sink;
            }));
    }

    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("text", run);

}  // namespace

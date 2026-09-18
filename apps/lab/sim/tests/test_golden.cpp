// SPDX-License-Identifier: GPL-3.0-or-later
//
// The recorded hashes of a fixed scenario. If a change makes this fail, decide whether it was
// meant to alter simulation results, and say so in the commit.

#include "lab_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstdio>

using atlas::lab::LabConfig;
using atlas::lab::testing::LabHarness;

namespace {

constexpr LabConfig kGoldenConfig{.width = 32, .height = 32, .chunk_size = 8, .seed = 0xA71A5};
constexpr std::uint64_t kGoldenKernelSeed = 0x0A71'A5'0000'0007ULL;
constexpr std::uint64_t kGoldenTicks = 500;
constexpr std::uint32_t kGoldenCommandsPerTick = 2;

struct Golden {
    std::uint64_t initial = 0;
    std::uint64_t final_state = 0;
    std::uint64_t all_ticks = 0;
};

[[nodiscard]] Golden run_golden() {
    LabHarness h(kGoldenConfig);
    auto kernel = h.kernel(kGoldenKernelSeed);
    Golden golden;
    golden.initial = h.lab.world.hash();
    atlas::Hasher over_time;
    for (std::uint64_t i = 0; i < kGoldenTicks; ++i) {
        REQUIRE(atlas::lab::submit_synthetic_commands(
                    h.commands, kernel.current_tick(), kGoldenCommandsPerTick, kGoldenKernelSeed,
                    h.lab.layout.cell_count(), atlas::sim::SourceId::Local)
                    .has_value());
        const auto report = kernel.step();
        REQUIRE(report.has_value());
        over_time.add(report->state_hash);
        golden.final_state = report->state_hash;
    }
    golden.all_ticks = over_time.value();
    return golden;
}

}  // namespace

TEST_CASE("the lab's fixed scenario produces its recorded hashes", "[lab][golden]") {
    const Golden golden = run_golden();
    std::printf("lab golden: initial=%#018llx final_state=%#018llx all_ticks=%#018llx\n",
                static_cast<unsigned long long>(golden.initial),
                static_cast<unsigned long long>(golden.final_state),
                static_cast<unsigned long long>(golden.all_ticks));
    // Recorded from the first run on macOS arm64, 2026-09-16. The Linux lanes confirm them.
    // Re-recorded in M8 for kHashAlgorithmVersion 2. Checked, not assumed: with the old hash
    // restored this scenario still produced 0xA0732541350B8590, 0x71D23497524806E3 and
    // 0x97765086AAA0C2C2, so the lab's simulation is unchanged and only the hash moved.
    CHECK(golden.initial == 0x9CF3'9EF6'6B2C'4845ULL);
    CHECK(golden.final_state == 0x1514'CC4B'3F41'D18EULL);
    CHECK(golden.all_ticks == 0xA962'B352'A643'ABC9ULL);
}

TEST_CASE("the lab's fixed scenario is stable within a run", "[lab][golden]") {
    const Golden first = run_golden();
    const Golden second = run_golden();
    CHECK(first.initial == second.initial);
    CHECK(first.final_state == second.final_state);
    CHECK(first.all_ticks == second.all_ticks);
}

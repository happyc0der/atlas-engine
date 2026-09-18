// SPDX-License-Identifier: GPL-3.0-or-later

/// \file
/// A fixed scenario whose hashes are written down.
///
/// The replay tests prove a run reproduces itself. This proves something narrower and harder:
/// that the values have not changed since they were recorded. A refactor that quietly alters
/// an ordering rule, a hash input, or the random mixer would keep every replay test passing,
/// because both sides of those comparisons move together. Only a written-down number catches
/// it.
///
/// The constants below were produced by this scenario and are not derived from anything. If a
/// change makes this test fail, that is the question being asked: was the change meant to
/// alter simulation results? If yes, record why and update the number. If no, the change has
/// a bug.
///
/// The same values are expected to hold across compilers and architectures here, because this
/// scenario is deliberately integer-only. That is a measurement rather than a promise; what
/// was measured, and on what, is in docs/DETERMINISM.md.

#include <atlas/core/assert.hpp>
#include <atlas/simulation/golden.hpp>
#include <atlas/simulation/kernel.hpp>

#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <vector>

using atlas::sim::Kernel;
using atlas::sim::KernelConfig;
using atlas::sim::kGoldenAllTicks;
using atlas::sim::kGoldenFinalState;
using atlas::sim::kGoldenRows;
using atlas::sim::kGoldenSeed;
using atlas::sim::kGoldenTicks;
using atlas::sim::testing::Harness;
using atlas::sim::testing::increment_values;
using atlas::sim::testing::random_into_counter;
using atlas::sim::testing::sum_into_counter;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

// The scenario and its recorded hashes are declared in <atlas/simulation/golden.hpp>. They
// moved there in M14: a lockstep handshake exchanges them to decide whether two builds can be
// compared at all, and a module cannot include a test.

struct Golden {
    std::uint64_t final_state = 0;
    std::uint64_t all_ticks = 0;
};

/// Run the scenario and reduce it to two numbers.
[[nodiscard]] Golden run_golden() {
    Harness h(kGoldenRows);
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
    REQUIRE(h.schedule.add(random_into_counter(h.counter)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.seed = kGoldenSeed});
    const auto reports = kernel.run(kGoldenTicks);
    REQUIRE(reports.has_value());

    // Every tick folded together, not only the last, so a divergence in the middle that
    // happens to converge again is still caught.
    atlas::Hasher over_time;
    for (const auto& report : *reports) {
        over_time.add(report.state_hash);
    }

    return Golden{
        .final_state = reports->back().state_hash,
        .all_ticks = over_time.value(),
    };
}

}  // namespace

TEST_CASE("the fixed scenario still produces its recorded hashes", "[sim][golden]") {
    const Golden golden = run_golden();

    // Printed as well as checked, so that a failure on a new platform hands over the value to
    // record rather than only saying that it differs.
    std::printf("golden scenario: final_state=%#018llx all_ticks=%#018llx\n",
                static_cast<unsigned long long>(golden.final_state),
                static_cast<unsigned long long>(golden.all_ticks));

    // Re-recorded in M8 when kHashAlgorithmVersion became 2. The change was meant to alter
    // hash values and nothing else, and that was checked rather than assumed: with the old
    // hash restored and everything else as it is now, this scenario still produced
    // 0xCECE73AEEC22FBCA and 0xD71CEC7C1078DD46, so the simulation's state is bit-identical
    // and only the function that reduces it to a number changed.
    CHECK(golden.final_state == kGoldenFinalState);
    CHECK(golden.all_ticks == kGoldenAllTicks);
}

TEST_CASE("the fixed scenario is stable within a run", "[sim][golden]") {
    const Golden first = run_golden();
    const Golden second = run_golden();
    CHECK(first.final_state == second.final_state);
    CHECK(first.all_ticks == second.all_ticks);
}

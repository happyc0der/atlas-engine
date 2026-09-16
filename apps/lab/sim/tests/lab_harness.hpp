// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A generated world with its schedule, command queue and bound wired the way the application
// wires them, so every test starts from the same composition the lab runs.

#include <atlas/core/assert.hpp>
#include <atlas/lab/commands.hpp>
#include <atlas/lab/generate.hpp>
#include <atlas/lab/systems.hpp>
#include <atlas/simulation/kernel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace atlas::lab::testing {

// The kernel asserts it is on the main thread, as the engine's own tests arrange. An inline
// variable, so the marking happens once however many translation units include this.
inline const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

struct LabHarness {
    LabWorld lab;
    sim::Schedule schedule;
    sim::CommandQueue commands;
    std::shared_ptr<CellBound> bound = std::make_shared<CellBound>(0);

    explicit LabHarness(const LabConfig& config) {
        auto generated = generate(config);
        REQUIRE(generated.has_value());
        lab = std::move(*generated);
        bound->store(lab.layout.cell_count());
        REQUIRE(add_lab_systems(schedule, lab.ids).has_value());
        REQUIRE(schedule.finalise(lab.world).has_value());
        REQUIRE(register_lab_commands(commands, lab.ids, bound).has_value());
    }

    [[nodiscard]] sim::Kernel kernel(std::uint64_t seed) {
        return sim::Kernel(lab.world, schedule, commands, sim::KernelConfig{.seed = seed});
    }
};

// Small enough to run five hundred ticks in a blink; big enough to have interior chunks.
inline constexpr LabConfig kSmall{.width = 16, .height = 16, .chunk_size = 4, .seed = 7};

}  // namespace atlas::lab::testing

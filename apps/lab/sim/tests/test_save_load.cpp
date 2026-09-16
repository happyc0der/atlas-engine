// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/save.hpp>

#include "lab_harness.hpp"
#include <catch2/catch_test_macros.hpp>

using atlas::lab::testing::kSmall;
using atlas::lab::testing::LabHarness;

TEST_CASE("a saved lab world loads back with the same hash", "[lab][save]") {
    LabHarness source(kSmall);
    auto kernel = source.kernel(5);
    REQUIRE(kernel.run(20).has_value());
    const auto bytes = atlas::sim::save(source.lab.world, kernel, source.commands).value();

    LabHarness target(kSmall);
    auto target_kernel = target.kernel(0);
    REQUIRE(atlas::sim::load(target.lab.world, target_kernel, target.commands, bytes).has_value());
    CHECK(target.lab.world.hash() == source.lab.world.hash());
    CHECK(target_kernel.current_tick() == 20);
    CHECK(atlas::lab::validate_world(target.lab.world, target.lab.ids).has_value());
}

TEST_CASE("save, load and continue matches an uninterrupted run", "[lab][save]") {
    // 200 ticks, save, load elsewhere, 100 more, against a single 300-tick run. With commands
    // on the way, because a replay without commands proves less than it appears to.
    const auto with_commands = [](atlas::sim::Kernel& kernel, LabHarness& h, std::uint64_t ticks) {
        for (std::uint64_t i = 0; i < ticks; ++i) {
            const atlas::Tick tick = kernel.current_tick();
            REQUIRE(atlas::lab::submit_synthetic_commands(h.commands, tick, 2, 77,
                                                          h.lab.layout.cell_count())
                        .has_value());
            REQUIRE(kernel.step().has_value());
        }
    };

    LabHarness straight(kSmall);
    auto straight_kernel = straight.kernel(9);
    with_commands(straight_kernel, straight, 300);

    LabHarness first(kSmall);
    auto first_kernel = first.kernel(9);
    with_commands(first_kernel, first, 200);
    const auto bytes = atlas::sim::save(first.lab.world, first_kernel, first.commands).value();

    LabHarness second(kSmall);
    auto second_kernel = second.kernel(0);
    REQUIRE(atlas::sim::load(second.lab.world, second_kernel, second.commands, bytes).has_value());
    REQUIRE(second_kernel.current_tick() == 200);
    with_commands(second_kernel, second, 100);

    CHECK(second.lab.world.hash() == straight.lab.world.hash());
}

TEST_CASE("a truncated save is refused and the world is unchanged", "[lab][save]") {
    LabHarness source(kSmall);
    auto kernel = source.kernel(5);
    REQUIRE(kernel.run(3).has_value());
    auto bytes = atlas::sim::save(source.lab.world, kernel, source.commands).value();
    bytes.resize(bytes.size() / 2);

    LabHarness target(kSmall);
    auto target_kernel = target.kernel(0);
    const auto before = target.lab.world.hash();
    CHECK_FALSE(
        atlas::sim::load(target.lab.world, target_kernel, target.commands, bytes).has_value());
    CHECK(target.lab.world.hash() == before);
}

TEST_CASE("a save whose grid row disagrees with its tables is caught after load", "[lab][save]") {
    LabHarness source(kSmall);
    auto kernel = source.kernel(5);
    // Still a valid layout on its own, so the grid row's own check passes; only the
    // cross-table check can see that the cells table does not match it.
    atlas::lab::grid_table(source.lab.world, source.lab.ids).width = 32;
    const auto bytes = atlas::sim::save(source.lab.world, kernel, source.commands).value();

    LabHarness target(kSmall);
    auto target_kernel = target.kernel(0);
    REQUIRE(atlas::sim::load(target.lab.world, target_kernel, target.commands, bytes).has_value());
    const auto validated = atlas::lab::validate_world(target.lab.world, target.lab.ids);
    REQUIRE_FALSE(validated.has_value());
    CHECK(validated.error().to_string().find("cells") != std::string::npos);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A chess world with an empty schedule and a command queue, wired the way the application
// will wire them, so every test starts from the composition the game runs.

#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/simulation/kernel.hpp>

#include <catch2/catch_test_macros.hpp>

namespace atlas::chess::testing {

// The kernel asserts it is on the main thread, as the engine's own tests arrange.
inline const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

struct ChessHarness {
    ChessWorld chess;
    sim::Schedule schedule;
    sim::CommandQueue commands;

    ChessHarness() {
        auto made = make_world();
        REQUIRE(made.has_value());
        chess = std::move(*made);
        // No systems: a position changes only through a command. The schedule still has to
        // be finalised, because the kernel refuses one that is not.
        REQUIRE(schedule.finalise(chess.world).has_value());
        REQUIRE(register_chess_commands(commands, chess.ids).has_value());
    }

    /// Submit a move for `tick` from `source`.
    [[nodiscard]] Status submit(Tick tick, Move move, sim::SourceId source = sim::SourceId::Local) {
        const auto payload = encode_move(move);
        return commands.submit(tick, source, kMoveCommand, payload);
    }

    [[nodiscard]] sim::Kernel kernel(std::uint64_t seed) {
        return sim::Kernel(chess.world, schedule, commands, sim::KernelConfig{.seed = seed});
    }

    [[nodiscard]] sim::World& world() noexcept { return chess.world; }

    [[nodiscard]] const TableIds& ids() const noexcept { return chess.ids; }
};

}  // namespace atlas::chess::testing

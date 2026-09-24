// SPDX-License-Identifier: GPL-3.0-or-later
//
// What chess costs the engine.
//
// Two scenarios:
//
//   chess/perft          — the legal move count to depth three from the starting position,
//                          8,902 leaves. The rules' own hot loop: generate every pseudo-legal
//                          move, make each on a copy, and ask whether the king is attacked.
//   chess/ply            — one move through the whole path a game takes: a `chess.move`
//                          command submitted, the kernel's tick, the handler's legality check,
//                          the tables rewritten, the endings asked, and the world hashed.
//
// **What is not measured is the board on screen.** A benchmark that drew it would be measuring
// a graphics device; M22's view is sixty-four squares and thirty-two pieces in one draw call.
//
// ---------------------------------------------------------------------------------------
// PREDICTION, written before the first run and committed before the numbers exist.
//
// `chess/perft` at depth three: **1 to 5 milliseconds**, or 0.1 to 0.6 µs a leaf. Depth three
// generates legal moves in 421 positions. Each is about thirty pseudo-legal moves, and each of
// those is a 72-byte copy, a scan of sixty-four squares for the king, and an attack test of up
// to eight rays — call it 100 to 300 ns — so 3 to 9 µs a position, and about 1 to 4 ms in all.
// Copy-make is chosen for being obviously correct, not fast; an engine that searched would
// be ten to fifty times quicker, and this one does not search.
//
// `chess/ply`: **10 to 40 µs**. Two full legal-move generations — one to judge the move, one
// to see whether the reply position is final — at 3 to 9 µs each, plus the kernel's tick and
// the hash of five small tables, which the lab's golden runs put at a few microseconds.
//
// **The claim this is expected to support is that a game costs the engine nothing worth
// reclaiming.** A ply at 40 µs is a quarter of a percent of a 16.6 ms frame, and a game makes
// one every few seconds. Above **200 µs** a ply, something is allocating per move — a move
// list on the heap, or the history growing without bound — which is the one thing worth
// finding out about.
//
// Being wrong here is fine and is why it is recorded first. Being unable to be wrong is not.
// ---------------------------------------------------------------------------------------

#include <atlas/chess/position.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/simulation/kernel.hpp>

#include "harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using atlas::bench::Result;

[[noreturn]] void die(std::string_view what) {
    std::fprintf(stderr, "bench_chess: %s\n", std::string{what}.c_str());
    std::abort();
}

std::vector<Result> run() {
    atlas::mark_main_thread();
    std::vector<Result> results;

    {
        const atlas::chess::Position start = atlas::chess::Position::start();
        if (atlas::chess::perft(start, 3) != 8'902) {
            die("perft disagrees with the published count");
        }
        results.push_back(atlas::bench::measure("chess/perft", "depth=3 leaves=8902", 200, 20, [&] {
            const volatile std::uint64_t sink = atlas::chess::perft(start, 3);
            (void)sink;
        }));
    }

    {
        auto made = atlas::chess::make_world();
        if (!made) {
            die("making the world");
        }
        atlas::chess::ChessWorld chess = std::move(*made);
        atlas::sim::Schedule schedule;
        atlas::sim::CommandQueue commands;
        if (!schedule.finalise(chess.world) ||
            !atlas::chess::register_chess_commands(commands, chess.ids)) {
            die("wiring the kernel");
        }
        atlas::sim::Kernel kernel(chess.world, schedule, commands, atlas::sim::KernelConfig{});
        const atlas::chess::Position start = atlas::chess::Position::start();
        const auto payload = atlas::chess::encode_move(
            {.from = atlas::chess::square(4, 1), .to = atlas::chess::square(4, 3)});

        // Each iteration puts the starting position back and plays e2e4. Resetting is inside
        // the timed region, and costs one table write and one key; it is small beside two legal
        // move generations, and a ply cannot be measured without a position to make it from.
        results.push_back(
            atlas::bench::measure("chess/ply", "e2e4 from the start", 20'000, 2'000, [&] {
                atlas::chess::set_position(chess.world, chess.ids, start);
                if (!commands.submit(kernel.current_tick(), atlas::sim::SourceId::Local,
                                     atlas::chess::kMoveCommand, payload)) {
                    die("submitting");
                }
                const auto report = kernel.step();
                if (!report || report->commands_applied != 1) {
                    die("the move was not applied");
                }
            }));
    }

    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("chess", run);

}  // namespace

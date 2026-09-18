// SPDX-License-Identifier: GPL-3.0-or-later
// Two simulations, one process, and the proof that they agree.
//
// This is M14's acceptance check at the engine level. Everything before it tested a part; this
// runs two kernels through the real peer interface — the real codec, the real inbox, the real
// gate — over a link that delays, reorders and breaks things on purpose, and compares what they
// produce **at every tick**. Not just the final hash: a run that diverged and converged again
// diverged, and a check that only looks at the end would call it a match.
#include <atlas/core/assert.hpp>
#include <atlas/net/loopback.hpp>
#include <atlas/net/session.hpp>
#include <atlas/simulation/golden.hpp>
#include <atlas/simulation/rng.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include "net_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <vector>

using atlas::net::LinkFault;
using atlas::net::LoopbackHub;
using atlas::net::Session;
using atlas::net::SessionConfig;
using atlas::net::testing::claim_payload;
using atlas::net::testing::kCells;
using atlas::net::testing::kClaimCell;
using atlas::net::testing::Peer;
using atlas::sim::Command;
using atlas::sim::Kernel;
using atlas::sim::KernelConfig;
using atlas::sim::RngStream;
using atlas::sim::stream_id;
using atlas::sim::TurnGate;

namespace {

const bool kMainThreadMarkedForLockstep = [] {
    atlas::mark_main_thread();
    return true;
}();

/// One participant: its simulation, its session, its gate, and what it produced.
struct Participant {
    Peer peer;
    TurnGate gate;
    std::unique_ptr<Session> session;
    std::unique_ptr<Kernel> kernel;
    std::vector<std::uint64_t> hashes;
    std::vector<std::size_t> applied;
    /// The next tick this peer has still to announce. Tracks its own kernel rather than the
    /// frame count, which is what keeps a stalled peer from running ahead in announcements.
    atlas::Tick next_turn = 0;
};

/// A whole session: N participants over one hub, driven together.
struct Table {
    std::unique_ptr<LoopbackHub> hub;
    std::vector<std::unique_ptr<Participant>> peers;
    std::uint32_t delay = 2;

    Table(std::size_t count, const atlas::net::LoopbackConfig& link, SessionConfig config) {
        auto made = LoopbackHub::create(link);
        REQUIRE(made.has_value());
        hub = *std::move(made);
        delay = config.input_delay;

        for (std::size_t i = 0; i < count; ++i) {
            auto participant = std::make_unique<Participant>();
            config.initial_state_hash = participant->peer.world.hash();
            auto session = Session::create(hub->end(i), config);
            REQUIRE(session.has_value());
            participant->session = *std::move(session);
            participant->session->set_schedule(&participant->peer.schedule);
            participant->kernel = std::make_unique<Kernel>(
                participant->peer.world, participant->peer.schedule, participant->peer.commands,
                KernelConfig{.seed = config.seed, .gate = &participant->gate});
            peers.push_back(std::move(participant));
        }
    }

    /// Poll every participant until each has agreed the session, or give up loudly.
    void settle() {
        for (int attempt = 0; attempt < 32; ++attempt) {
            bool all_running = true;
            for (auto& participant : peers) {
                REQUIRE(participant->session->poll(0, participant->peer.commands, participant->gate)
                            .has_value());
                all_running = all_running && participant->session->running();
            }
            if (all_running) {
                return;
            }
        }
        FAIL("the session never agreed");
    }

    /// One frame for every participant: poll, announce a turn, then run whatever is ready.
    ///
    /// Returns false when any participant's session has ended, so a caller can tell a stall
    /// from a failure without inspecting both.
    [[nodiscard]] bool frame(bool submit_commands = true) {
        for (auto& participant : peers) {
            if (!participant->session->running()) {
                return false;
            }
            if (!participant->session->poll(participant->kernel->current_tick(),
                                            participant->peer.commands, participant->gate)) {
                return false;
            }
        }

        // Announce every turn this peer owes, and no more.
        //
        // The tick to announce is **this peer's own current tick plus the delay**, not a frame
        // counter. A first version counted frames, and a stalled peer went on announcing turns
        // for ticks it had not run — so its partner ran ahead, sent hash checks for ticks the
        // stalled peer had never reached, and the session ended on a rule that was working
        // correctly. Tracking the kernel throttles a stalled peer naturally, which is also what
        // makes the mutual stall below happen at all.
        //
        // No gaps, starting at the first tick: the gate advances a frontier over consecutive
        // turns, so the first `delay` ticks must be announced even though nobody will ever have
        // commands for them.
        for (std::size_t i = 0; i < peers.size(); ++i) {
            auto& participant = *peers[i];
            const auto source = participant.session->self();
            const atlas::Tick horizon = participant.kernel->current_tick() + delay;
            while (participant.next_turn <= horizon) {
                const atlas::Tick tick = participant.next_turn;
                std::vector<Command> commands;
                if (submit_commands && tick >= delay) {
                    // **Both peers claim the same cell**, with different claimants. One stream,
                    // keyed by the tick, so the choice is reproducible and identical on both.
                    //
                    // Contention every tick is the point. With each peer picking its own cell
                    // the two commands commute, and a mutation destroying the total order
                    // survives the whole proof — which it did, until this line. Only a tick
                    // where the order decides the outcome can show that the order is agreed.
                    RngStream stream(9001, stream_id("contested cell"), tick);
                    const auto cell = static_cast<std::uint32_t>(stream.next_below(kCells));
                    Command command{
                        .target = tick,
                        .source = source,
                        .sequence = participant.peer.commands.next_sequence(source),
                        .type = kClaimCell,
                        .payload = claim_payload(cell, static_cast<std::uint32_t>(i) + 1),
                    };
                    // Submitted locally as well as sent: a peer applies its own commands like
                    // everybody else's, through the queue, in the total order.
                    REQUIRE(participant.peer.commands.submit_stamped(command).has_value());
                    commands.push_back(std::move(command));
                }
                if (!participant.session->send_turn(tick, commands, participant.gate)) {
                    return false;
                }
                ++participant.next_turn;
            }
        }

        // Everyone polls again so the turns just sent are available before anyone steps.
        for (auto& participant : peers) {
            if (!participant->session->poll(participant->kernel->current_tick(),
                                            participant->peer.commands, participant->gate)) {
                return false;
            }
        }

        for (auto& participant : peers) {
            while (participant->kernel->ready()) {
                const auto report = participant->kernel->step();
                if (!report) {
                    return false;
                }
                participant->hashes.push_back(report->state_hash);
                participant->applied.push_back(report->commands_applied);
                participant->gate.retire_before(report->tick);
                if (!participant->session->send_hash_check(report->tick, report->state_hash,
                                                           report->system_hashes)) {
                    return false;
                }
            }
        }
        return true;
    }
};

}  // namespace

TEST_CASE("two kernels agree at every tick over an unkind link", "[net][lockstep]") {
    // Latency, reordering, and two genuinely different command streams. The link is doing
    // everything it can short of losing something, and the peers still reach the same state at
    // every tick — which is the whole claim of the milestone.
    Table table(2, {.peer_count = 2, .latency_polls = 2, .reorder = true, .reorder_seed = 7},
                SessionConfig{.input_delay = 2, .hash_check_interval = 8, .seed = 4242});
    table.settle();

    for (int frame = 0; frame < 400; ++frame) {
        INFO("frame " << frame);
        REQUIRE(table.frame());
    }

    auto& a = *table.peers[0];
    auto& b = *table.peers[1];
    REQUIRE(a.hashes.size() > 100);
    REQUIRE(a.hashes.size() == b.hashes.size());
    // Tick by tick, not just at the end: a run that diverged and converged again diverged.
    for (std::size_t tick = 0; tick < a.hashes.size(); ++tick) {
        INFO("tick " << tick);
        REQUIRE(a.hashes[tick] == b.hashes[tick]);
        REQUIRE(a.applied[tick] == b.applied[tick]);
    }

    // Not vacuous: commands actually crossed the link and were applied, and hashes were
    // actually compared. Without these the case passes on a session in which nothing happened.
    CHECK(a.session->stats().turns_received > 100);
    CHECK(a.session->stats().commands_received > 100);
    CHECK(a.session->stats().hash_checks_agreed > 10);
    CHECK_FALSE(a.session->divergence().has_value());
}

TEST_CASE("a state-dependent refusal is identical on both peers", "[net][lockstep]") {
    // The case the original brief could not have: corrupting a payload cannot prove identical
    // rejection, because the sender already applied the original. This is the genuine version —
    // both peers claim cells, the total order decides which claim wins, and **the refusal is
    // itself recorded in the state**. A rejection that were a silent no-op would be
    // indistinguishable from the command never arriving, and this case would pass on two peers
    // that received nothing.
    Table table(2, {.peer_count = 2, .latency_polls = 1, .reorder = true, .reorder_seed = 3},
                SessionConfig{.input_delay = 2, .hash_check_interval = 8, .seed = 11});
    table.settle();
    for (int frame = 0; frame < 300; ++frame) {
        REQUIRE(table.frame());
    }

    const auto* cells_a = dynamic_cast<const atlas::net::testing::CellTable*>(
        table.peers[0]->peer.world.table(table.peers[0]->peer.cells));
    const auto* cells_b = dynamic_cast<const atlas::net::testing::CellTable*>(
        table.peers[1]->peer.world.table(table.peers[1]->peer.cells));
    REQUIRE(cells_a != nullptr);
    REQUIRE(cells_b != nullptr);

    std::uint64_t refusals = 0;
    std::uint64_t owned = 0;
    for (std::size_t cell = 0; cell < kCells; ++cell) {
        refusals += cells_a->contested[cell];
        owned += cells_a->owner[cell] != 0 ? 1 : 0;
        INFO("cell " << cell);
        // Cell by cell rather than by the summary hash: two worlds can hash alike and differ,
        // and the whole claim here is that the *outcome of a contest* matches.
        CHECK(cells_a->owner[cell] == cells_b->owner[cell]);
        CHECK(cells_a->contested[cell] == cells_b->contested[cell]);
        // And *who* was refused, not merely that somebody was.
        CHECK(cells_a->last_refused[cell] == cells_b->last_refused[cell]);
        // And the winner is always the lower source, on both peers. That is not a preference —
        // it is the total order `(source, sequence)` being the order, visible in the state. A
        // peer resolving the contest by arrival would win some of these and lose others.
        CHECK(cells_a->owner[cell] != 2);
    }
    // Claims were refused, and cells were won: otherwise this is a test of a world nobody
    // contested, which would pass on two peers that received nothing from each other.
    CHECK(refusals > 0);
    CHECK(owned > 0);
}

TEST_CASE("a held turn stalls the session rather than diverging it", "[net][lockstep]") {
    // The property that separates lockstep from hope. A turn that has not arrived must stop
    // every peer at that tick — not be skipped, not be guessed at — and releasing it must let
    // both finish in the same state.
    Table table(2, {.peer_count = 2},
                SessionConfig{.input_delay = 2, .hash_check_interval = 8, .seed = 77});
    table.settle();
    for (int frame = 0; frame < 60; ++frame) {
        REQUIRE(table.frame());
    }
    const std::size_t before = table.peers[0]->hashes.size();
    REQUIRE(before > 0);

    // Peer one's next turn is parked. Peer zero stops, and then peer one stops too — because
    // peer zero stopped ticking and so stopped sending turns of its own. That mutual stall is
    // the property, and it is worth stating: lockstep does not degrade gracefully, it waits.
    REQUIRE(table.hub->arm_fault(1, 0, LinkFault::Hold).has_value());
    for (int frame = 0; frame < 40; ++frame) {
        REQUIRE(table.frame());
    }
    CHECK(table.peers[0]->hashes.size() < before + 40);
    CHECK(table.hub->held_count() == 1);
    // Stalled, not broken: nothing diverged and both sessions are still running.
    CHECK_FALSE(table.peers[0]->session->divergence().has_value());
    CHECK(table.peers[0]->session->running());
    CHECK(table.peers[1]->session->running());

    table.hub->release_held();
    for (int frame = 0; frame < 120; ++frame) {
        REQUIRE(table.frame());
    }

    auto& a = *table.peers[0];
    auto& b = *table.peers[1];
    CHECK(a.hashes.size() > before);
    REQUIRE(a.hashes.size() == b.hashes.size());
    for (std::size_t tick = 0; tick < a.hashes.size(); ++tick) {
        INFO("tick " << tick);
        REQUIRE(a.hashes[tick] == b.hashes[tick]);
    }
}

TEST_CASE("a corrupted message never produces a silent disagreement", "[net][lockstep]") {
    // One flipped bit in a message body, and the property asserted is the one that matters:
    // **the peers never end up quietly holding different states**. Either the damage is caught —
    // refused at decode, rejected as a protocol violation, or detected by a hash check — or it
    // landed somewhere that changes nothing and the peers still agree.
    //
    // Stated that way rather than as "the session stops", because in this fixture the flip
    // often does land inertly: it hits the high byte of a claimant whose claim is about to be
    // refused anyway, or a per-system hash, which is carried for attribution and never
    // compared. Asserting a stop would be asserting something that is not reliably true, and
    // the version of this case that did assert it passed only because `stopped` happened to be
    // set by a different message. The lab's own `loopback_corrupt_turn_is_caught` covers the
    // detected path end to end, where a damaged `set_color_index` always changes a cell.
    Table table(2, {.peer_count = 2},
                SessionConfig{.input_delay = 2, .hash_check_interval = 4, .seed = 5});
    table.settle();
    for (int frame = 0; frame < 30; ++frame) {
        REQUIRE(table.frame());
    }

    REQUIRE(table.hub->arm_fault(0, 1, LinkFault::Corrupt).has_value());
    bool stopped = false;
    for (int frame = 0; frame < 60 && !stopped; ++frame) {
        stopped = !table.frame();
    }

    // The fault fired: without this the case passes on a link that did nothing.
    CHECK(table.hub->stats().corrupted == 1);

    if (stopped) {
        // Caught. Somebody refused it, and no peer carried on with a state nobody else has.
        CHECK_FALSE(table.peers[0]->session->running());
    } else {
        // Not caught, which means it changed nothing — and that must be provable rather than
        // assumed. Compared as the whole world, so a difference in any table shows.
        CHECK(table.peers[0]->peer.world.hash() == table.peers[1]->peer.world.hash());
        CHECK(table.peers[0]->kernel->current_tick() == table.peers[1]->kernel->current_tick());
    }
}

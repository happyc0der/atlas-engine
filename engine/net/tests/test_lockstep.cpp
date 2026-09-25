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

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using atlas::net::LinkFault;
using atlas::net::LoopbackHub;
using atlas::net::Session;
using atlas::net::SessionConfig;
using atlas::net::testing::claim_payload;
using atlas::net::testing::kCells;
using atlas::net::testing::kClaimCell;
using atlas::net::testing::kClaimIfFree;
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
    std::vector<std::size_t> declined;
    /// The next tick this peer has still to announce. Tracks its own kernel rather than the
    /// frame count, which is what keeps a stalled peer from running ahead in announcements.
    atlas::Tick next_turn = 0;
    /// Every drop its session reported, in the order reported (ADR-0022).
    std::vector<atlas::sim::DroppedSource> drops;
    /// Gone, as a killed process is: never polled, never stepped, never announces again.
    bool absent = false;
    /// Why its session last refused a poll, so a failing case can say.
    std::string error;
};

/// A whole session: N participants over one hub, driven together.
struct Table {
    std::unique_ptr<LoopbackHub> hub;
    std::vector<std::unique_ptr<Participant>> peers;
    std::uint32_t delay = 2;
    /// Which claim every peer submits: the recorded contest by default, or the declining one.
    atlas::sim::CommandType claim_type = kClaimCell;
    /// When set, no peer runs a tick at or past this one, so a finish can be placed exactly.
    std::optional<atlas::Tick> stop_at;

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
            if (participant->absent) {
                continue;
            }
            if (!participant->session->running()) {
                participant->error = "the session is not running";
                return false;
            }
            if (!poll_and_record(*participant)) {
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
            if (participant.absent) {
                continue;
            }
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
                        .type = claim_type,
                        .payload = claim_payload(cell, static_cast<std::uint32_t>(i) + 1),
                    };
                    // Submitted locally as well as sent: a peer applies its own commands like
                    // everybody else's, through the queue, in the total order.
                    REQUIRE(participant.peer.commands.submit_stamped(command).has_value());
                    commands.push_back(std::move(command));
                }
                if (auto sent = participant.session->send_turn(tick, commands, participant.gate);
                    !sent) {
                    participant.error = sent.error().message();
                    return false;
                }
                ++participant.next_turn;
            }
        }

        // Everyone polls again so the turns just sent are available before anyone steps.
        for (auto& participant : peers) {
            if (participant->absent) {
                continue;
            }
            if (!poll_and_record(*participant)) {
                return false;
            }
        }

        for (auto& participant : peers) {
            if (participant->absent) {
                continue;
            }
            while (participant->kernel->ready() &&
                   (!stop_at.has_value() || participant->kernel->current_tick() < *stop_at)) {
                const auto report = participant->kernel->step();
                if (!report) {
                    participant->error = report.error().message();
                    return false;
                }
                participant->hashes.push_back(report->state_hash);
                participant->applied.push_back(report->commands_applied);
                participant->declined.push_back(report->commands_declined);
                participant->gate.retire_before(report->tick);
                if (auto checked = participant->session->send_hash_check(
                        report->tick, report->state_hash, report->system_hashes);
                    !checked) {
                    participant->error = checked.error().message();
                    return false;
                }
            }
        }
        return true;
    }

    /// Frames until every peer has reached `tick`, or give up loudly.
    void run_to(atlas::Tick tick) {
        stop_at = tick;
        for (int frame = 0; frame < 2000; ++frame) {
            if (std::ranges::all_of(peers, [tick](const auto& p) {
                    return p->absent || p->kernel->current_tick() >= tick;
                })) {
                // One more, so every peer has announced its turns `delay` ticks past the stop.
                REQUIRE(this->frame());
                return;
            }
            REQUIRE(this->frame());
        }
        FAIL("the peers never reached tick " << tick);
    }

    /// Run one peer's next tick, if the gate allows it, and send its hash check.
    [[nodiscard]] static bool step_one(Participant& participant) {
        if (!participant.kernel->ready()) {
            return false;
        }
        const auto report = participant.kernel->step();
        REQUIRE(report.has_value());
        participant.hashes.push_back(report->state_hash);
        participant.gate.retire_before(report->tick);
        REQUIRE(participant.session
                    ->send_hash_check(report->tick, report->state_hash, report->system_hashes)
                    .has_value());
        return true;
    }

    /// Poll one peer, keeping any drop it reports.
    [[nodiscard]] static bool poll_and_record(Participant& participant) {
        auto report = participant.session->poll(participant.kernel->current_tick(),
                                                participant.peer.commands, participant.gate);
        if (!report) {
            participant.error = report.error().message();
            return false;
        }
        participant.drops.insert(participant.drops.end(), report->dropped.begin(),
                                 report->dropped.end());
        return true;
    }

    /// Poll one peer, returning the report or the error.
    [[nodiscard]] static atlas::Result<atlas::sim::PollReport> poll_one(Participant& participant) {
        return participant.session->poll(participant.kernel->current_tick(),
                                         participant.peer.commands, participant.gate);
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

TEST_CASE("a decline is made identically by both peers and leaves no trace in the state",
          "[net][lockstep]") {
    // The case M14 could not write (ADR-0019). Its state-dependent refusal had to be recorded
    // in the world to be observable; this one records nothing, and the kernel's own count is
    // what is compared. Both peers claim the same cell every tick, so the total order decides
    // whose claim is applied and whose is declined, and the two must agree on that — tick by
    // tick, in the count and in the hash — or lockstep has a hole in it.
    Table table(2, {.peer_count = 2, .latency_polls = 1, .reorder = true, .reorder_seed = 5},
                SessionConfig{.input_delay = 2, .hash_check_interval = 8, .seed = 17});
    table.claim_type = kClaimIfFree;
    table.settle();
    for (int frame = 0; frame < 300; ++frame) {
        REQUIRE(table.frame());
    }

    auto& a = *table.peers[0];
    auto& b = *table.peers[1];
    REQUIRE(a.hashes.size() > 100);
    REQUIRE(a.hashes.size() == b.hashes.size());
    std::size_t declined_total = 0;
    for (std::size_t tick = 0; tick < a.hashes.size(); ++tick) {
        INFO("tick " << tick);
        REQUIRE(a.hashes[tick] == b.hashes[tick]);
        REQUIRE(a.applied[tick] == b.applied[tick]);
        REQUIRE(a.declined[tick] == b.declined[tick]);
        declined_total += a.declined[tick];
    }

    // Not vacuous: once every cell is owned, every claim is declined, so most of the run is
    // declines — and the session saw nothing wrong with any of them.
    CHECK(declined_total > 100);
    CHECK(a.kernel->declined_commands() == declined_total);
    CHECK(a.kernel->invalid_commands() == 0);
    CHECK(a.session->stats().hash_checks_agreed > 10);
    CHECK_FALSE(a.session->divergence().has_value());

    // And nothing about a decline reached the state: every cell that is owned was claimed once,
    // and the contest counters the recording fixture uses stay at zero.
    const auto* cells =
        dynamic_cast<const atlas::net::testing::CellTable*>(a.peer.world.table(a.peer.cells));
    REQUIRE(cells != nullptr);
    for (std::size_t cell = 0; cell < kCells; ++cell) {
        CHECK(cells->contested[cell] == 0);
        CHECK(cells->last_refused[cell] == 0);
    }
}

TEST_CASE("a declined claim leaves the world as a twin that never saw it", "[net][lockstep]") {
    // The contract behind the case above, checked directly: a handler that declines has
    // changed nothing. Two peers run in isolation; both claim the cell once, and one is then
    // sent a second claim on the same cell, which is declined. Its hash afterwards must equal
    // the twin's, or the decline wrote something on its way out.
    Peer declined;
    Peer twin;
    Kernel kernel(declined.world, declined.schedule, declined.commands, KernelConfig{.seed = 3});
    Kernel kernel_twin(twin.world, twin.schedule, twin.commands, KernelConfig{.seed = 3});

    for (auto* commands : {&declined.commands, &twin.commands}) {
        REQUIRE(commands->submit(0, atlas::sim::SourceId::Local, kClaimIfFree, claim_payload(4, 1))
                    .has_value());
    }
    REQUIRE(
        declined.commands.submit(1, atlas::sim::SourceId::Local, kClaimIfFree, claim_payload(4, 2))
            .has_value());

    REQUIRE(kernel.step().has_value());
    REQUIRE(kernel_twin.step().has_value());
    const auto report = kernel.step();
    const auto twin_report = kernel_twin.step();
    REQUIRE(report.has_value());
    REQUIRE(twin_report.has_value());

    CHECK(report->commands_declined == 1);
    CHECK(report->commands_applied == 0);
    CHECK(report->state_hash == twin_report->state_hash);
}

// ------------------------------------------------------------------------------------ finishing
//
// ADR-0020. A session ends because its run is over, and each clause of "over" has a case here
// that removing it would fail: every partner said where it finishes and at the same tick, this
// peer has run that tick, and the last hash check at or before it has been compared.

namespace {

using atlas::net::SessionState;

/// Two peers, delay two, checks every eight ticks, no latency games: these cases are about
/// the finish, and the link's unkindness is covered above.
[[nodiscard]] Table finishing_table() {
    return Table(2, {.peer_count = 2, .latency_polls = 0, .reorder = false},
                 SessionConfig{.input_delay = 2, .hash_check_interval = 8, .seed = 21});
}

/// Poll both peers `rounds` times, requiring every poll to succeed.
void poll_both(Table& table, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        for (auto& participant : table.peers) {
            REQUIRE(Table::poll_one(*participant).has_value());
        }
    }
}

}  // namespace

TEST_CASE("two peers that finish at the same tick both report it, and it is not a failure",
          "[net][lockstep][finish]") {
    Table table = finishing_table();
    table.settle();
    table.run_to(25);  // ticks 0..24 run
    auto& a = *table.peers[0];
    auto& b = *table.peers[1];

    REQUIRE(a.session->finish(24).has_value());
    CHECK(a.session->state() == SessionState::Finishing);
    CHECK_FALSE(a.session->running());
    REQUIRE(b.session->finish(24).has_value());

    poll_both(table, 4);
    CHECK(a.session->finished());
    CHECK(b.session->finished());
    CHECK(a.hashes == b.hashes);

    // Finished is a state a poll reports rather than an error it returns, on every poll after.
    const auto again = Table::poll_one(a);
    REQUIRE(again.has_value());
    CHECK(again->finished);
    CHECK_FALSE(again->closed);
}

TEST_CASE("a peer is not finished until its partner says where it finishes",
          "[net][lockstep][finish]") {
    Table table = finishing_table();
    table.settle();
    table.run_to(25);
    auto& a = *table.peers[0];
    auto& b = *table.peers[1];

    REQUIRE(a.session->finish(24).has_value());
    poll_both(table, 8);
    CHECK(a.session->state() == SessionState::Finishing);
    CHECK(b.session->state() == SessionState::Running);
    // Peer 1 has not finished, and knows where peer 0 did: what a driver compares against its
    // own idea of how long the run was.
    CHECK(a.session->declared_finish() == atlas::Tick{24});
    CHECK(b.session->declared_finish() == atlas::Tick{24});

    REQUIRE(b.session->finish(24).has_value());
    poll_both(table, 4);
    CHECK(a.session->finished());
    CHECK(b.session->finished());
}

TEST_CASE("a peer is not finished until it has run its last tick", "[net][lockstep][finish]") {
    // Finishing may be declared early — the turns are announced — but "over" means the last
    // tick has run here. This clause is also the turn clause: the gate lets a tick run only
    // once every partner's turn for it has arrived.
    //
    // **The last tick is off the check interval on purpose.** A first draft finished at 24,
    // which is a check tick, and a mutation removing this clause survived it: the hash-check
    // clause also waits for tick 24 to run, and masked the one under test. At 26 the last check
    // is 24, already run and compared, so nothing but this clause stands in the way.
    Table table = finishing_table();
    table.settle();
    table.run_to(26);  // ticks 0..25 run; 26 announced
    auto& a = *table.peers[0];
    auto& b = *table.peers[1];

    REQUIRE(a.session->finish(26).has_value());
    REQUIRE(b.session->finish(26).has_value());
    poll_both(table, 6);
    CHECK(a.session->state() == SessionState::Finishing);
    CHECK(b.session->state() == SessionState::Finishing);

    REQUIRE(Table::step_one(a));
    REQUIRE(Table::step_one(b));
    poll_both(table, 4);
    CHECK(a.session->finished());
    CHECK(b.session->finished());
}

TEST_CASE("a peer is not finished until its partner's last hash check has been compared",
          "[net][lockstep][finish]") {
    // Tick 24 is on the interval, so it is the last check. Peer 1's check for it is held on the
    // way to peer 0; peer 1 learns everything it needs and finishes, peer 0 cannot — it has not
    // compared the last stretch of the run — until the check is released. Without this clause
    // peer 0 would report as agreed a run whose final hashes it never saw.
    Table table = finishing_table();
    table.settle();
    table.run_to(24);
    auto& a = *table.peers[0];
    auto& b = *table.peers[1];

    REQUIRE(table.hub->arm_fault(1, 0, LinkFault::Hold).has_value());
    REQUIRE(Table::step_one(b));  // its check for tick 24 is the message held
    REQUIRE(Table::step_one(a));
    REQUIRE(a.session->finish(24).has_value());
    REQUIRE(b.session->finish(24).has_value());

    poll_both(table, 8);
    CHECK(b.session->finished());
    CHECK(a.session->state() == SessionState::Finishing);

    // And a finished peer's goodbye is not sent: its partner is still finishing, and would
    // otherwise read a completed run as a peer that left.
    b.session->quit();
    CHECK(b.session->finished());

    table.hub->release_held();
    poll_both(table, 4);
    CHECK(a.session->finished());
}

TEST_CASE("two peers that finish at different ticks end the session", "[net][lockstep][finish]") {
    // They were told to run different games. No later message can reconcile that, so it is a
    // protocol violation on both sides rather than a wait.
    Table table = finishing_table();
    table.settle();
    table.run_to(25);
    auto& a = *table.peers[0];
    auto& b = *table.peers[1];

    REQUIRE(a.session->finish(24).has_value());
    REQUIRE(b.session->finish(25).has_value());  // announced through 26, so this is allowed

    const auto from_a = Table::poll_one(a);
    const auto from_b = Table::poll_one(b);
    REQUIRE_FALSE(from_a.has_value());
    REQUIRE_FALSE(from_b.has_value());
    CHECK(from_a.error().message().contains("disagree"));
    CHECK(a.session->state() == SessionState::Ended);
    CHECK(b.session->state() == SessionState::Ended);
}

TEST_CASE("a peer that runs past its partner's finish ends the session",
          "[net][lockstep][finish]") {
    // The other half of the mismatch. Peer 0 finishes at 24; peer 1 had already been told the
    // turns for 25 and 26, runs 24 and 25, and at its next poll has run a tick nobody else will.
    Table table = finishing_table();
    table.settle();
    table.run_to(24);
    auto& a = *table.peers[0];
    auto& b = *table.peers[1];

    REQUIRE(Table::step_one(a));
    REQUIRE(a.session->finish(24).has_value());
    REQUIRE(Table::poll_one(b).has_value());  // the finish arrives; tick 24 is not yet past
    REQUIRE(Table::step_one(b));
    REQUIRE(Table::step_one(b));  // tick 25: past the finish

    const auto refused = Table::poll_one(b);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("already run tick 25"));
    CHECK_FALSE(Table::poll_one(a).has_value());  // and peer 0 hears that it left
}

TEST_CASE("finish is refused before the session runs and past what was announced",
          "[net][lockstep][finish]") {
    Table unsettled = finishing_table();
    CHECK_FALSE(unsettled.peers[0]->session->declared_finish().has_value());
    const auto early = unsettled.peers[0]->session->finish(0);
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error().code() == atlas::ErrorCode::Unavailable);

    Table table = finishing_table();
    table.settle();
    table.run_to(24);  // announced through 26
    const auto ahead = table.peers[0]->session->finish(40);
    REQUIRE_FALSE(ahead.has_value());
    CHECK(ahead.error().code() == atlas::ErrorCode::InvalidArgument);
    // Refused, not ended: the peer can still finish somewhere it has announced.
    CHECK(table.peers[0]->session->running());
    CHECK(table.peers[0]->session->finish(26).has_value());
}

// ---------------------------------------------------------------------------------------------
// Dropping a lost peer (ADR-0022). Three kernels over a star, so every message reaches the others
// through peer zero, which is what the agreement rests on.

namespace {

[[nodiscard]] atlas::net::LoopbackConfig star_link(std::uint64_t reorder_seed) {
    return {.peer_count = 3,
            .latency_polls = 2,
            .reorder = true,
            .reorder_seed = reorder_seed,
            .topology = atlas::net::Topology::Star};
}

[[nodiscard]] SessionConfig dropping(std::uint64_t seed) {
    return SessionConfig{.input_delay = 2,
                         .hash_check_interval = 8,
                         .seed = seed,
                         .on_peer_lost = atlas::net::PeerLoss::Drop};
}

/// Peers 0 and 1 reached the same state at every tick either of them ran.
void require_agreement(const Participant& a, const Participant& b) {
    const std::size_t common = std::min(a.hashes.size(), b.hashes.size());
    REQUIRE(common > 100);
    for (std::size_t tick = 0; tick < common; ++tick) {
        INFO("tick " << tick);
        REQUIRE(a.hashes[tick] == b.hashes[tick]);
        REQUIRE(a.applied[tick] == b.applied[tick]);
    }
}

}  // namespace

TEST_CASE("three kernels agree at every tick through a relay", "[net][lockstep][drop]") {
    // The star on its own, before anybody is lost: peer zero forwards, the others never meet,
    // and all three still reach the same state at every tick.
    Table table(3, star_link(5), SessionConfig{.input_delay = 2, .hash_check_interval = 8});
    table.settle();
    for (int frame = 0; frame < 300; ++frame) {
        INFO("frame " << frame);
        REQUIRE(table.frame());
    }
    require_agreement(*table.peers[0], *table.peers[1]);
    require_agreement(*table.peers[0], *table.peers[2]);
    // Peer 1 heard from peer 2 without any direct connection between them.
    CHECK(table.peers[1]->session->stats().turns_received > 200);
    CHECK(table.hub->stats().sent > 0);
}

TEST_CASE("a lost peer is dropped, and the others agree at every tick after",
          "[net][lockstep][drop]") {
    Table table(3, star_link(7), dropping(31));
    table.settle();
    for (int frame = 0; frame < 60; ++frame) {
        REQUIRE(table.frame());
    }
    const auto turns_before = table.peers[1]->session->stats().turns_received;

    // Peer 2 dies, as a killed process does: what it had in flight never arrives.
    REQUIRE(table.hub->lose(2).has_value());
    table.peers[2]->absent = true;

    for (int frame = 0; frame < 300; ++frame) {
        const bool ok = table.frame();
        INFO("frame " << frame << ": " << table.peers[0]->error << " | " << table.peers[1]->error);
        REQUIRE(ok);
    }

    auto& a = *table.peers[0];
    auto& b = *table.peers[1];
    require_agreement(a, b);
    // Well past the point where peer 2 stopped: the session went on rather than stalling.
    CHECK(a.hashes.size() > table.peers[2]->hashes.size() + 100);

    // Both dropped the same peer at the same point, once.
    REQUIRE(a.drops.size() == 1);
    REQUIRE(b.drops.size() == 1);
    CHECK(a.drops.front().source == atlas::sim::SourceId{2});
    CHECK(b.drops.front().source == atlas::sim::SourceId{2});
    CHECK(a.drops.front().first_missing_turn == b.drops.front().first_missing_turn);
    CHECK(a.session->peers().size() == 2);
    CHECK(b.session->peers().size() == 2);
    // Not vacuous: peer 2's turns really did reach peer 1, through the relay, before it went.
    CHECK(turns_before > 60);
    CHECK_FALSE(a.session->divergence().has_value());
}

TEST_CASE("a gap in the lost peer's turns does not split the others", "[net][lockstep][drop]") {
    // The case "drop at the highest tick the relay holds" would get wrong. One of peer 2's
    // turns to the relay is parked, later ones arrive past it, and then peer 2 is lost, taking
    // the parked one with it. The relay holds a set with a hole in it — and so does every other
    // peer, because the relay forwarded exactly what it received. The drop is agreed on the set,
    // by counting it, rather than on a tick.
    Table table(3, star_link(11), dropping(47));
    table.settle();
    for (int frame = 0; frame < 40; ++frame) {
        REQUIRE(table.frame());
    }
    REQUIRE(table.hub->arm_fault(2, 0, LinkFault::Hold).has_value());
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(table.frame());
    }
    REQUIRE(table.hub->held_count() == 1);
    REQUIRE(table.hub->lose(2).has_value());
    table.peers[2]->absent = true;
    CHECK(table.hub->held_count() == 0);

    for (int frame = 0; frame < 300; ++frame) {
        INFO("frame " << frame);
        REQUIRE(table.frame());
    }
    require_agreement(*table.peers[0], *table.peers[1]);
    REQUIRE(table.peers[0]->drops.size() == 1);
    REQUIRE(table.peers[1]->drops.size() == 1);
    CHECK(table.peers[0]->drops.front().first_missing_turn ==
          table.peers[1]->drops.front().first_missing_turn);
}

TEST_CASE("a peer that says goodbye is dropped rather than ending everyone",
          "[net][lockstep][drop]") {
    Table table(3, star_link(13), dropping(59));
    table.settle();
    for (int frame = 0; frame < 50; ++frame) {
        REQUIRE(table.frame());
    }
    table.peers[2]->session->quit();
    table.peers[2]->absent = true;

    for (int frame = 0; frame < 200; ++frame) {
        INFO("frame " << frame);
        REQUIRE(table.frame());
    }
    require_agreement(*table.peers[0], *table.peers[1]);
    CHECK(table.peers[0]->drops.size() == 1);
    CHECK(table.peers[1]->drops.size() == 1);
}

TEST_CASE("under the default policy a lost peer still ends the session", "[net][lockstep][drop]") {
    // ADR-0017 D5 is still the default: nothing about a relay changes it.
    Table table(3, star_link(17), SessionConfig{.input_delay = 2, .hash_check_interval = 8});
    table.settle();
    for (int frame = 0; frame < 30; ++frame) {
        REQUIRE(table.frame());
    }
    REQUIRE(table.hub->lose(2).has_value());
    table.peers[2]->absent = true;

    bool ended = false;
    for (int frame = 0; frame < 20 && !ended; ++frame) {
        ended = !table.frame();
    }
    CHECK(ended);
    CHECK(table.peers[0]->drops.empty());
}

TEST_CASE("dropping a peer needs a link that relays", "[net][lockstep][drop]") {
    // On a mesh one peer can hold a turn of the lost one that nobody else ever will, and no
    // count the others agree on can fix that.
    auto hub = LoopbackHub::create({.peer_count = 3});
    REQUIRE(hub.has_value());
    const auto refused = Session::create((*hub)->end(0), dropping(1));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("relays"));
}

TEST_CASE("the peers left after a drop finish without it", "[net][lockstep][drop][finish]") {
    // A dropped peer owes nothing: not its turns, not its finish, not its hashes. A finish
    // condition that still counted it would wait for ever on a peer that is gone (ADR-0022 D7).
    Table table(3, star_link(19), dropping(71));
    table.settle();
    for (int frame = 0; frame < 30; ++frame) {
        REQUIRE(table.frame());
    }
    REQUIRE(table.hub->lose(2).has_value());
    table.peers[2]->absent = true;
    table.run_to(80);

    auto& a = *table.peers[0];
    auto& b = *table.peers[1];
    REQUIRE(a.drops.size() == 1);
    REQUIRE(a.session->finish(79).has_value());
    REQUIRE(b.session->finish(79).has_value());
    for (int round = 0; round < 6; ++round) {
        REQUIRE(Table::poll_one(a).has_value());
        REQUIRE(Table::poll_one(b).has_value());
    }
    CHECK(a.session->finished());
    CHECK(b.session->finished());
    CHECK(a.hashes == b.hashes);
}

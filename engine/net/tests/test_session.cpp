// SPDX-License-Identifier: GPL-3.0-or-later
// One session's behaviour: the handshake, and what it refuses.
//
// Driven with a loopback hub and two real sessions, but with no kernel: what is being checked
// here is what the session does with what arrives, not whether two simulations agree. That is
// proved separately, and keeping them apart is what makes a failure in either one legible.
#include <atlas/core/assert.hpp>
#include <atlas/net/session.hpp>
#include <atlas/simulation/golden.hpp>
#include <atlas/simulation/replay.hpp>
#include <atlas/simulation/save.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <vector>

using atlas::net::CommandInbox;
using atlas::net::encode;
using atlas::net::HashCheck;
using atlas::net::Hello;
using atlas::net::LoopbackHub;
using atlas::net::Message;
using atlas::net::Session;
using atlas::net::SessionConfig;
using atlas::net::SessionState;
using atlas::net::Turn;
using atlas::sim::Command;
using atlas::sim::command_type;
using atlas::sim::CommandHandler;
using atlas::sim::CommandQueue;
using atlas::sim::CommandType;
using atlas::sim::SourceId;
using atlas::sim::SystemHash;
using atlas::sim::SystemId;
using atlas::sim::TurnGate;

namespace {

const bool kMainThreadMarkedForSession = [] {
    atlas::mark_main_thread();
    return true;
}();

const CommandType kPoke = command_type("poke");

[[nodiscard]] CommandHandler poke_handler() {
    CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 1) {
            return std::unexpected(
                atlas::Error(atlas::ErrorCode::MalformedData, "expected one byte"));
        }
        return atlas::ok();
    };
    handler.apply = [](atlas::sim::World&, std::span<const std::byte>) {};
    return handler;
}

/// Two peers, each with its own queue and gate, wired through one hub.
struct Pair {
    std::unique_ptr<LoopbackHub> hub;
    std::unique_ptr<Session> a;
    std::unique_ptr<Session> b;
    CommandQueue queue_a;
    CommandQueue queue_b;
    TurnGate gate_a;
    TurnGate gate_b;

    explicit Pair(const SessionConfig& config = {}) {
        auto made = LoopbackHub::create({.peer_count = 2});
        REQUIRE(made.has_value());
        hub = *std::move(made);

        REQUIRE(queue_a.register_handler(kPoke, poke_handler()).has_value());
        REQUIRE(queue_b.register_handler(kPoke, poke_handler()).has_value());

        auto first = Session::create(hub->end(0), config);
        REQUIRE(first.has_value());
        a = *std::move(first);
        auto second = Session::create(hub->end(1), config);
        REQUIRE(second.has_value());
        b = *std::move(second);
    }

    /// Poll both until the handshake completes, or give up rather than spin.
    void settle(atlas::Tick now = 0) {
        for (int attempt = 0; attempt < 16 && !(a->running() && b->running()); ++attempt) {
            REQUIRE(a->poll(now, queue_a, gate_a).has_value());
            REQUIRE(b->poll(now, queue_b, gate_b).has_value());
        }
        REQUIRE(a->running());
        REQUIRE(b->running());
    }
};

[[nodiscard]] Command poke(atlas::Tick target, SourceId source, std::uint64_t sequence) {
    return Command{
        .target = target,
        .source = source,
        .sequence = sequence,
        .type = kPoke,
        .payload = {std::byte{1}},
    };
}

}  // namespace

TEST_CASE("a session refuses a configuration that could not work", "[net][session]") {
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    // An input delay of zero stamps a command for the tick already running, so it is late on
    // arrival everywhere else — a session that would end on its first turn.
    CHECK_FALSE(Session::create((*hub)->end(0), {.input_delay = 0}).has_value());
    // And an interval of zero compares nothing, which is a session with no divergence detection
    // dressed up as one that has it.
    CHECK_FALSE(Session::create((*hub)->end(0), {.hash_check_interval = 0}).has_value());
}

TEST_CASE("two peers agree and each learns its own identity", "[net][session]") {
    Pair pair;
    pair.settle();

    // Local is a role. Peer one must not stamp it, or it would be signing peer zero's name to
    // its own commands and the two streams would collide in the total order.
    CHECK(pair.a->self() == SourceId{0});
    CHECK(pair.b->self() == SourceId{1});
    CHECK(pair.a->self() == SourceId::Local);
    CHECK(pair.b->self() != SourceId::Local);

    CHECK(pair.a->peers().size() == 2);
    CHECK(pair.gate_a.expected_sources().size() == 2);
    // And nothing is ready yet: both peers are expected and neither has spoken for tick zero.
    CHECK_FALSE(pair.gate_a.ready(0));
}

TEST_CASE("the agreed delay is the largest anybody proposed", "[net][session]") {
    // Not the first peer's, and not the smallest. A peer given less delay than it needs is late
    // every tick, and late ends a session.
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    //
    // The larger proposal is deliberately the **first** peer's. An earlier version of this case
    // had it second, where "take the largest" and "take whichever came last" agree — and a
    // mutation replacing one with the other survived. Order is the thing being ruled out, so
    // the test has to put the answer where a positional rule would miss it.
    auto first = Session::create((*hub)->end(0), {.input_delay = 7});
    REQUIRE(first.has_value());
    auto second = Session::create((*hub)->end(1), {.input_delay = 2});
    REQUIRE(second.has_value());

    CommandQueue queue_a;
    CommandQueue queue_b;
    TurnGate gate_a;
    TurnGate gate_b;
    for (int attempt = 0; attempt < 16; ++attempt) {
        REQUIRE((*first)->poll(0, queue_a, gate_a).has_value());
        REQUIRE((*second)->poll(0, queue_b, gate_b).has_value());
    }
    REQUIRE((*first)->running());
    CHECK((*first)->agreed_delay() == 7);
    CHECK((*second)->agreed_delay() == 7);
}

TEST_CASE("a turn carries commands into the queue and marks its sender's gate", "[net][session]") {
    Pair pair;
    pair.settle();

    // A turn for every tick, which is what a session actually does. A turn for tick five on its
    // own says nothing about ticks zero to four, and the gate is right to keep waiting for them
    // — a lesson this case learned by asserting otherwise first.
    for (atlas::Tick tick = 0; tick < 5; ++tick) {
        REQUIRE(pair.b->send_turn(tick, {}, pair.gate_b).has_value());
    }
    const std::array<Command, 2> commands{poke(5, SourceId{1}, 0), poke(5, SourceId{1}, 1)};
    REQUIRE(pair.b->send_turn(5, commands, pair.gate_b).has_value());
    // The sender marks its own turns without waiting for them to come back: a peer does not
    // need the network to learn what it did. Checked as "peer one is not the one being waited
    // for", because the horizon is the minimum across every source and peer zero has said
    // nothing on this gate at all.
    std::vector<SourceId> waiting_locally;
    pair.gate_b.waiting_on(5, waiting_locally);
    REQUIRE(waiting_locally.size() == 1);
    CHECK(waiting_locally.front() == SourceId{0});

    const auto report = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE(report.has_value());
    CHECK(report->commands_submitted == 2);
    CHECK(report->turns_marked == 6);
    CHECK(report->highest_target == 5);
    CHECK(pair.queue_a.pending() == 2);

    // Peer one's turns are recorded; peer zero has said nothing, so nothing is ready.
    CHECK_FALSE(pair.gate_a.ready(0));
    std::vector<SourceId> waiting;
    pair.gate_a.waiting_on(5, waiting);
    REQUIRE(waiting.size() == 1);
    CHECK(waiting.front() == SourceId{0});
}

TEST_CASE("an empty turn is sent and marks the gate", "[net][session]") {
    // Silence is what a dead peer produces, and a gate with no timeout cannot tell the two
    // apart. A peer with nothing to say still says it.
    Pair pair;
    pair.settle();

    REQUIRE(pair.b->send_turn(0, {}, pair.gate_b).has_value());
    const auto report = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE(report.has_value());
    CHECK(report->commands_submitted == 0);
    CHECK(report->turns_marked == 1);
}

TEST_CASE("a peer cannot submit a turn on another peer's behalf", "[net][session]") {
    // The source is taken from the link the message arrived on, never from the message. A peer
    // that could write another's name could complete its turns for it, and the gate would open
    // on a tick whose commands were never sent.
    Pair pair;
    pair.settle();

    Turn forged{.tick = 1, .source = SourceId{0}, .commands = {}};
    const auto bytes = encode(Message{std::move(forged)});
    REQUIRE(bytes.has_value());
    auto end = pair.hub->end(1);
    REQUIRE(end.send_to(0, *bytes).has_value());

    const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK_FALSE(pair.a->running());
    CHECK(pair.a->state() == SessionState::Ended);
}

TEST_CASE("a peer cannot label a command inside its own turn with another peer's source",
          "[net][session]") {
    // The envelope check above is not enough on its own. A turn may legitimately come from
    // peer 1 while a command inside it claims to be peer 0 — and `submit_stamped` keeps the
    // sequence number it is given, so that command can collide with a real one from peer 0 on
    // `(source, sequence)`. The order `drain` sorts by would then not be total, and two peers
    // could order the pair differently and diverge with nothing logged. Fatal, like the
    // envelope, because it is the same act one level down.
    Pair pair;
    pair.settle();

    Turn smuggled{.tick = 1,
                  .source = SourceId{1},
                  .commands = {poke(1, SourceId{1}, 0), poke(1, SourceId{0}, 7)}};
    const auto bytes = encode(Message{std::move(smuggled)});
    REQUIRE(bytes.has_value());
    auto end = pair.hub->end(1);
    REQUIRE(end.send_to(0, *bytes).has_value());

    const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("may speak only for itself"));
    CHECK_FALSE(pair.a->running());

    // Checked before anything was submitted, so the honest command in front of the forged one
    // did not reach the queue either. A turn is one message and one event: half of an invalid
    // turn must not be left behind for the next tick to apply.
    CHECK(pair.queue_a.pending() == 0);
}

TEST_CASE("a mod's identifier cannot arrive over a link", "[net][session]") {
    // ADR-0014: a mod's commands are local to each peer and never sent, because every peer runs
    // the same mods and produces the same commands for itself. Nothing in the session says the
    // word "mod" — the property falls out of taking the source from the link a message arrived
    // on rather than from the message. Written down as a test because that is the kind of
    // guarantee which survives only as long as somebody remembers it is load-bearing.
    Pair pair;
    pair.settle();

    Turn smuggled{
        .tick = 1, .source = SourceId{1}, .commands = {poke(1, atlas::sim::mod_source(0), 0)}};
    const auto bytes = encode(Message{std::move(smuggled)});
    REQUIRE(bytes.has_value());
    auto end = pair.hub->end(1);
    REQUIRE(end.send_to(0, *bytes).has_value());

    const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("may speak only for itself"));
    CHECK(pair.queue_a.pending() == 0);
}

TEST_CASE("a turn for a tick already run ends the session", "[net][session]") {
    // Under lockstep a late command cannot be applied by anybody, so the session is no longer
    // sound. Checked before the queue sees it, so the kernel never counts it late — which is
    // what the assertion on late_commands is for.
    Pair pair;
    pair.settle();

    const std::array<Command, 1> commands{poke(3, SourceId{1}, 0)};
    REQUIRE(pair.b->send_turn(3, commands, pair.gate_b).has_value());

    const auto refused = pair.a->poll(10, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("already running"));
    CHECK_FALSE(pair.a->running());
    // Nothing reached the queue, so nothing can be counted late later on.
    CHECK(pair.queue_a.pending() == 0);
}

TEST_CASE("a peer this build cannot be compared with is refused at the handshake",
          "[net][session]") {
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    auto session = Session::create((*hub)->end(0), {});
    REQUIRE(session.has_value());

    CommandQueue queue;
    TurnGate gate;

    SECTION("a different golden hash") {
        // The probe the charter puts on a cross-build session. Two builds agreeing here agree
        // about the simulation whatever else differs between them; two that do not, do not.
        Hello theirs;
        theirs.hash_algorithm_version = atlas::kHashAlgorithmVersion;
        theirs.save_format_version = atlas::sim::kSaveFormatVersion;
        theirs.replay_format_version = atlas::sim::kReplayFormatVersion;
        theirs.golden_final_state = atlas::sim::kGoldenFinalState ^ 1ULL;
        theirs.golden_all_ticks = atlas::sim::kGoldenAllTicks;
        theirs.tick_rate = 60;
        theirs.proposed_delay = 2;
        const auto bytes = encode(Message{theirs});
        REQUIRE(bytes.has_value());
        auto end = (*hub)->end(1);
        REQUIRE(end.send_to(0, *bytes).has_value());

        const auto refused = (*session)->poll(0, queue, gate);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == atlas::ErrorCode::VersionMismatch);
        CHECK(refused.error().message().contains("fixed scenario"));
    }

    SECTION("a different starting world") {
        // Not a divergence to detect later: a session that was never going to be fair. Refused
        // up front, exactly as a replay from the wrong starting state is.
        Hello theirs;
        theirs.hash_algorithm_version = atlas::kHashAlgorithmVersion;
        theirs.save_format_version = atlas::sim::kSaveFormatVersion;
        theirs.replay_format_version = atlas::sim::kReplayFormatVersion;
        theirs.golden_final_state = atlas::sim::kGoldenFinalState;
        theirs.golden_all_ticks = atlas::sim::kGoldenAllTicks;
        theirs.initial_state_hash = 0x1234;
        theirs.tick_rate = 60;
        theirs.proposed_delay = 2;
        const auto bytes = encode(Message{theirs});
        REQUIRE(bytes.has_value());
        auto end = (*hub)->end(1);
        REQUIRE(end.send_to(0, *bytes).has_value());

        const auto refused = (*session)->poll(0, queue, gate);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == atlas::ErrorCode::VersionMismatch);
        CHECK(refused.error().message().contains("initial state hash"));
    }
}

TEST_CASE("a hash check for a tick this peer never checked ends the session", "[net][session]") {
    // Skipping it would make the divergence detector quietly vacuous: a peer checking on a
    // different schedule would agree with everybody by never being compared with anybody.
    Pair pair;
    pair.settle();

    HashCheck stray{.tick = 7, .source = SourceId{1}, .state_hash = 99, .system_hashes = {}};
    const auto bytes = encode(Message{std::move(stray)});
    REQUIRE(bytes.has_value());
    auto end = pair.hub->end(1);
    REQUIRE(end.send_to(0, *bytes).has_value());

    const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("did not check"));
}

TEST_CASE("agreeing hashes are counted and disagreeing ones stop the session", "[net][session]") {
    Pair pair;
    pair.settle();

    const std::array<SystemHash, 1> systems{SystemHash{SystemId{7}, 42}};

    SECTION("agreement") {
        REQUIRE(pair.a->send_hash_check(0, 0xABCD, systems).has_value());
        REQUIRE(pair.b->send_hash_check(0, 0xABCD, systems).has_value());
        REQUIRE(pair.a->poll(0, pair.queue_a, pair.gate_a).has_value());
        CHECK(pair.a->stats().hash_checks_agreed == 1);
        CHECK(pair.a->running());
        CHECK_FALSE(pair.a->divergence().has_value());
    }

    SECTION("disagreement") {
        const std::array<SystemHash, 1> other{SystemHash{SystemId{7}, 99}};
        REQUIRE(pair.a->send_hash_check(0, 0xABCD, systems).has_value());
        REQUIRE(pair.b->send_hash_check(0, 0x1234, other).has_value());

        const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(pair.a->divergence().has_value());
        CHECK(pair.a->divergence()->tick == 0);
        // Named with the peer as well as the system: in a session of more than two, "system X
        // differs" without "between me and peer N" is not a starting point for anybody.
        CHECK(pair.a->divergence()->description.contains("peer 1"));
        CHECK_FALSE(pair.a->running());
    }
}

TEST_CASE("a hash check is only sent on the agreed interval", "[net][session]") {
    Pair pair{SessionConfig{.hash_check_interval = 4}};
    pair.settle();

    for (atlas::Tick tick = 0; tick < 8; ++tick) {
        REQUIRE(pair.a->send_hash_check(tick, tick, {}).has_value());
    }
    // Ticks 0 and 4, and no others. Calling it every tick is correct and costs nothing on the
    // ticks it skips, which is what lets the caller not think about it.
    CHECK(pair.a->stats().hash_checks_sent == 2);
}

TEST_CASE("a farewell ends the session at the other end", "[net][session]") {
    Pair pair;
    pair.settle();

    pair.b->quit();
    CHECK(pair.b->state() == SessionState::Ended);

    const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("left"));
    CHECK_FALSE(pair.a->running());
}

TEST_CASE("a message that cannot be decoded ends the session", "[net][session]") {
    Pair pair;
    pair.settle();

    auto end = pair.hub->end(1);
    REQUIRE(end.send_to(0, std::vector<std::byte>(40, std::byte{0x7F})).has_value());

    const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK_FALSE(pair.a->running());
}

TEST_CASE("a peer that overran its inbox cannot be resumed", "[net][session]") {
    // A lockstep stream with a hole in it is worthless: the commands that fell out were going
    // to be applied on every other peer. Resuming would produce a divergence attributed to
    // whichever system happened to touch the missing command's table.
    Pair pair;
    pair.settle();

    auto end = pair.hub->end(1);
    const auto bytes = encode(Message{Turn{.tick = 1, .source = SourceId{1}, .commands = {}}});
    REQUIRE(bytes.has_value());
    for (std::size_t i = 0; i < CommandInbox::kMaxMessages + 10; ++i) {
        REQUIRE(end.send_to(0, *bytes).has_value());
    }

    const auto refused = pair.a->poll(0, pair.queue_a, pair.gate_a);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == atlas::ErrorCode::Exhausted);
    CHECK_FALSE(pair.a->running());
}

TEST_CASE("one peer's backlog does not starve another in the same poll", "[net][session]") {
    // Back-pressure rather than an error. What it buys is that a turn from a quiet peer is
    // still delivered in the poll a loud peer is filling, and a turn nobody reads is a tick
    // nobody runs.
    auto hub = LoopbackHub::create({.peer_count = 3});
    REQUIRE(hub.has_value());
    std::array<std::unique_ptr<Session>, 3> sessions;
    std::array<CommandQueue, 3> queues;
    std::array<TurnGate, 3> gates;
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(queues[i].register_handler(kPoke, poke_handler()).has_value());
        auto made = Session::create((*hub)->end(i), {});
        REQUIRE(made.has_value());
        sessions[i] = *std::move(made);
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        for (std::size_t i = 0; i < 3; ++i) {
            REQUIRE(sessions[i]->poll(0, queues[i], gates[i]).has_value());
        }
    }
    REQUIRE(sessions[0]->running());

    // Peer one repeats its turn for tick zero two hundred times; peer two sends its once.
    const auto chatter = encode(Message{Turn{.tick = 0, .source = SourceId{1}, .commands = {}}});
    REQUIRE(chatter.has_value());
    auto loud = (*hub)->end(1);
    for (std::size_t i = 0; i < 200; ++i) {
        REQUIRE(loud.send_to(0, *chatter).has_value());
    }
    const auto quiet_turn = encode(Message{Turn{.tick = 0, .source = SourceId{2}, .commands = {}}});
    REQUIRE(quiet_turn.has_value());
    auto quiet = (*hub)->end(2);
    REQUIRE(quiet.send_to(0, *quiet_turn).has_value());

    REQUIRE(sessions[0]->poll(0, queues[0], gates[0]).has_value());
    // Both peers' turns for tick zero are recorded in that single poll, so the quiet one was
    // not held up behind two hundred repetitions from the loud one.
    std::vector<SourceId> waiting;
    gates[0].waiting_on(0, waiting);
    REQUIRE(waiting.size() == 1);
    CHECK(waiting.front() == SourceId{0});

    // And the loud peer's backlog was not consumed whole: the cap stopped part way and the rest
    // is waiting for the next poll. Without this the case passes just as happily on a session
    // with no cap at all, which is a test of nothing — the same shape of gap that
    // `seed_changes_hash` exists to close in the lab.
    CHECK((*hub)->end(0).inbox(1).depth() > 0);
}

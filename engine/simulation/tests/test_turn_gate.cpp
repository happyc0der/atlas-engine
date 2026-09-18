// SPDX-License-Identifier: GPL-3.0-or-later
// The turn gate, driven directly.
//
// Every case here is about one of two things: that readiness depends only on which sources have
// reported, and that a peer cannot make the gate grow. Both are claims the header makes in
// prose, and prose is not a test.
#include <atlas/simulation/turn_gate.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using atlas::ErrorCode;
using atlas::Tick;
using atlas::sim::SourceGate;
using atlas::sim::SourceId;
using atlas::sim::TurnGate;

namespace {

/// Two peers, which is the smallest session where waiting means anything.
[[nodiscard]] TurnGate two_peers() {
    TurnGate gate;
    const std::array<SourceId, 2> peers{SourceId{0}, SourceId{1}};
    REQUIRE(gate.expect_sources(peers).has_value());
    return gate;
}

}  // namespace

TEST_CASE("a gate expecting nobody is ready for every tick", "[sim][gate]") {
    // The solo configuration, and the reason the golden hashes are unaffected by any of this.
    // A default gate must be indistinguishable from no gate at all.
    const TurnGate gate;
    CHECK(gate.ready(0));
    CHECK(gate.ready(1));
    CHECK(gate.ready(1'000'000));
    CHECK(gate.ready(std::numeric_limits<Tick>::max()));
    CHECK(gate.ready_horizon() == TurnGate::kUnboundedHorizon);
    CHECK(gate.expected_sources().empty());
}

TEST_CASE("a tick is ready once every expected source has completed it", "[sim][gate]") {
    TurnGate gate = two_peers();
    CHECK_FALSE(gate.ready(0));

    REQUIRE(gate.mark_complete(SourceId{0}, 0).has_value());
    // One of two is not enough, which is the entire point of the class.
    CHECK_FALSE(gate.ready(0));
    CHECK(gate.ready_horizon() == 0);

    REQUIRE(gate.mark_complete(SourceId{1}, 0).has_value());
    CHECK(gate.ready(0));
    CHECK(gate.ready_horizon() == 1);
    CHECK_FALSE(gate.ready(1));
}

TEST_CASE("readiness does not depend on how long anything took", "[sim][gate]") {
    // There is no clock to advance, so this cannot be tested by waiting. What can be tested is
    // that asking repeatedly never changes the answer: a gate that consulted a clock would have
    // to produce a different answer eventually, and one that does not, cannot.
    TurnGate gate = two_peers();
    REQUIRE(gate.mark_complete(SourceId{0}, 0).has_value());

    for (int attempt = 0; attempt < 10'000; ++attempt) {
        REQUIRE_FALSE(gate.ready(0));
        REQUIRE(gate.ready_horizon() == 0);
    }

    REQUIRE(gate.mark_complete(SourceId{1}, 0).has_value());
    CHECK(gate.ready(0));
}

TEST_CASE("marking a turn twice is not an error and does not move the horizon", "[sim][gate]") {
    // A lossy transport retransmits. A retransmission that produced an error would turn one
    // peer's ordinary retry into a log full of failures on every other peer.
    TurnGate gate = two_peers();
    REQUIRE(gate.mark_complete(SourceId{0}, 0).has_value());
    const Tick after_first = gate.ready_horizon();

    CHECK(gate.mark_complete(SourceId{0}, 0).has_value());
    CHECK(gate.mark_complete(SourceId{0}, 0).has_value());
    CHECK(gate.ready_horizon() == after_first);
}

TEST_CASE("marks arriving out of order leave the same state as marks in order", "[sim][gate]") {
    // Messages arrive in whatever order the network chose. Requiring ascending marks would make
    // readiness depend on arrival order, which is the one thing the gate must not do.
    TurnGate scrambled = two_peers();
    TurnGate ordered = two_peers();

    for (const Tick tick : {Tick{9}, Tick{7}, Tick{8}}) {
        REQUIRE(scrambled.mark_complete(SourceId{0}, tick).has_value());
    }
    for (const Tick tick : {Tick{7}, Tick{8}, Tick{9}}) {
        REQUIRE(ordered.mark_complete(SourceId{0}, tick).has_value());
    }

    // Compared across the whole range rather than at one point: a window that absorbed the
    // marks differently would agree about the horizon and disagree about a tick inside it.
    CHECK(scrambled.ready_horizon() == ordered.ready_horizon());
    for (Tick tick = 0; tick < 16; ++tick) {
        INFO("tick " << tick);
        CHECK(scrambled.ready(tick) == ordered.ready(tick));
    }

    // Ticks 0 to 6 were never marked, so the frontier cannot have moved past them however the
    // marks arrived. Without this the case would pass on a gate that took the highest mark.
    CHECK(scrambled.ready_horizon() == 0);
}

TEST_CASE("a run of marks with a gap absorbs only up to the gap", "[sim][gate]") {
    TurnGate gate = two_peers();
    for (const Tick tick : {Tick{0}, Tick{1}, Tick{3}}) {
        REQUIRE(gate.mark_complete(SourceId{0}, tick).has_value());
    }
    // Tick 2 is missing, so this source's frontier stops there whatever came after it.
    REQUIRE(gate.mark_complete(SourceId{1}, 0).has_value());
    REQUIRE(gate.mark_complete(SourceId{1}, 1).has_value());
    REQUIRE(gate.mark_complete(SourceId{1}, 2).has_value());
    CHECK(gate.ready(0));
    CHECK(gate.ready(1));
    CHECK_FALSE(gate.ready(2));

    // And filling the gap releases everything behind it in one step.
    REQUIRE(gate.mark_complete(SourceId{0}, 2).has_value());
    REQUIRE(gate.mark_complete(SourceId{1}, 3).has_value());
    CHECK(gate.ready(3));
    CHECK(gate.ready_horizon() == 4);
}

TEST_CASE("a source nobody expected cannot complete a turn", "[sim][gate]") {
    TurnGate gate = two_peers();
    const auto refused = gate.mark_complete(SourceId{99}, 0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::InvalidArgument);
    // And the turn is no more complete than it was.
    CHECK_FALSE(gate.ready(0));
}

TEST_CASE("a mark far past a source's frontier is refused as hostile", "[sim][gate]") {
    TurnGate gate = two_peers();

    // The last tick inside the window is accepted.
    REQUIRE(gate.mark_complete(SourceId{0}, TurnGate::kLookahead - 1).has_value());
    const std::size_t before = gate.footprint_bytes();

    // One further is not. Accepting it would mean either storing marks for an unbounded span of
    // ticks, or — far worse — taking it to mean every tick in between was complete too.
    const auto refused = gate.mark_complete(SourceId{0}, TurnGate::kLookahead);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::OutOfRange);

    // Ten thousand more claims cost nothing, which is what makes the refusal a bound rather
    // than a complaint.
    for (Tick tick = TurnGate::kLookahead; tick < TurnGate::kLookahead + 10'000; ++tick) {
        CHECK_FALSE(gate.mark_complete(SourceId{0}, tick).has_value());
    }
    CHECK(gate.footprint_bytes() == before);
    CHECK(gate.ready_horizon() == 0);
}

TEST_CASE("a silent peer costs nothing however long the other runs", "[sim][gate]") {
    // The memory claim the header makes, checked rather than believed. Source 1 never reports;
    // source 0 marks as far as it is allowed to.
    TurnGate gate = two_peers();
    const std::size_t empty = gate.footprint_bytes();

    for (Tick tick = 0; tick < 100'000; ++tick) {
        (void)gate.mark_complete(SourceId{0}, tick);
    }
    CHECK(gate.footprint_bytes() == empty);
    // Source 0 got exactly as far as the window allows and no further, because nothing retired
    // anything behind it.
    CHECK(gate.ready_horizon() == 0);
}

TEST_CASE("retiring moves the line below which a mark is a duplicate", "[sim][gate]") {
    TurnGate gate = two_peers();
    for (const SourceId source : {SourceId{0}, SourceId{1}}) {
        for (Tick tick = 0; tick < 4; ++tick) {
            REQUIRE(gate.mark_complete(source, tick).has_value());
        }
    }
    REQUIRE(gate.ready_horizon() == 4);

    gate.retire_before(4);
    CHECK(gate.floor() == 4);
    // A tick from before the floor has already run, so asking about it is answered rather than
    // refused, and a retransmitted mark for it is accepted and does nothing.
    CHECK(gate.ready(0));
    CHECK(gate.mark_complete(SourceId{0}, 0).has_value());
    CHECK(gate.ready_horizon() == 4);
}

TEST_CASE("the floor only ever moves forward", "[sim][gate]") {
    // Retiring is monotonic. A floor that moved backwards would be a session claiming it had
    // un-run a tick, and the visible damage lands on whoever joins next: a source arriving after
    // the floor slipped would start owing turns for ticks that had already been run by everyone
    // else, and the whole session would wait for them.
    //
    // This case exists because a mutation replacing the maximum with a plain assignment survived
    // every other test here. Nothing else retires twice.
    TurnGate gate = two_peers();
    for (const SourceId source : {SourceId{0}, SourceId{1}}) {
        for (Tick tick = 0; tick < 6; ++tick) {
            REQUIRE(gate.mark_complete(source, tick).has_value());
        }
    }

    gate.retire_before(5);
    CHECK(gate.floor() == 5);
    // Permitted — it is below the horizon — and ignored.
    gate.retire_before(2);
    CHECK(gate.floor() == 5);

    const std::array<SourceId, 3> three{SourceId{0}, SourceId{1}, SourceId{2}};
    REQUIRE(gate.expect_sources(three).has_value());
    // The newcomer starts at 5, not at 2: it owes nothing for ticks the session has finished.
    std::vector<SourceId> waiting;
    gate.waiting_on(4, waiting);
    CHECK(waiting.empty());
    CHECK(gate.ready_horizon() == 5);
}

TEST_CASE("adding a peer does not un-complete the peers already agreed", "[sim][gate]") {
    // The two already here are deliberately left **ahead of the floor**. An earlier version of
    // this case retired to exactly the tick they had reached, which made a mutation that resets
    // every source's progress on re-expectation invisible: the value it reset them to happened
    // to be the value they already had. The gap between the floor and their frontier is what
    // makes the property observable at all.
    TurnGate gate = two_peers();
    for (const SourceId source : {SourceId{0}, SourceId{1}}) {
        for (Tick tick = 0; tick < 4; ++tick) {
            REQUIRE(gate.mark_complete(source, tick).has_value());
        }
    }
    REQUIRE(gate.ready(3));
    gate.retire_before(2);

    const std::array<SourceId, 3> three{SourceId{0}, SourceId{1}, SourceId{2}};
    REQUIRE(gate.expect_sources(three).has_value());

    // The newcomer starts at the floor, so it alone owes ticks 2 and 3 and it is the only
    // source the gate is waiting on for either.
    std::vector<SourceId> waiting;
    gate.waiting_on(2, waiting);
    REQUIRE(waiting.size() == 1);
    CHECK(waiting.front() == SourceId{2});
    CHECK(gate.ready_horizon() == 2);

    // And once it catches up, the ticks the other two had already completed are ready
    // immediately, without either of them marking anything again. That is the property: a peer
    // joining does not make the peers already agreed re-do their turns.
    REQUIRE(gate.mark_complete(SourceId{2}, 2).has_value());
    REQUIRE(gate.mark_complete(SourceId{2}, 3).has_value());
    CHECK(gate.ready(2));
    CHECK(gate.ready(3));
    CHECK(gate.ready_horizon() == 4);
}

TEST_CASE("a duplicate in the expectation set is refused, not collapsed", "[sim][gate]") {
    TurnGate gate;
    const std::array<SourceId, 3> repeated{SourceId{0}, SourceId{1}, SourceId{0}};
    const auto refused = gate.expect_sources(repeated);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("an expectation set past the limit is refused", "[sim][gate]") {
    TurnGate gate;
    std::vector<SourceId> many;
    for (std::size_t i = 0; i <= TurnGate::kMaxSources; ++i) {
        many.push_back(SourceId{static_cast<std::uint32_t>(i)});
    }
    const auto refused = gate.expect_sources(many);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::OutOfRange);

    // And exactly at the limit is accepted, so the bound is the bound rather than one below it.
    many.pop_back();
    CHECK(gate.expect_sources(many).has_value());
}

TEST_CASE("what a tick is waiting on is reported in identifier order", "[sim][gate]") {
    // Ordered rather than merely enumerated: two peers comparing stall logs are comparing text,
    // and text assembled from an unordered container is not comparable.
    TurnGate gate;
    const std::array<SourceId, 3> peers{SourceId{5}, SourceId{2}, SourceId{9}};
    REQUIRE(gate.expect_sources(peers).has_value());
    REQUIRE(gate.mark_complete(SourceId{5}, 0).has_value());

    std::vector<SourceId> waiting;
    gate.waiting_on(0, waiting);
    REQUIRE(waiting.size() == 2);
    CHECK(waiting[0] == SourceId{2});
    CHECK(waiting[1] == SourceId{9});

    CHECK(gate.expected_sources().size() == 3);
    CHECK(gate.expected_sources()[0] == SourceId{2});

    // The buffer is cleared rather than appended to, or a caller reusing it once a frame would
    // report every source it has ever waited on.
    gate.waiting_on(0, waiting);
    CHECK(waiting.size() == 2);
}

TEST_CASE("a source gate can mark its own turns and no others", "[sim][gate]") {
    // The rule is enforced by the type rather than by a comment: there is no way to name
    // another source through this handle at all, which is what M15's untrusted mod needs.
    TurnGate gate = two_peers();
    SourceGate mine(gate, SourceId{1});
    CHECK(mine.source() == SourceId{1});
    REQUIRE(mine.mark_complete(0).has_value());

    std::vector<SourceId> waiting;
    gate.waiting_on(0, waiting);
    REQUIRE(waiting.size() == 1);
    CHECK(waiting.front() == SourceId{0});
}

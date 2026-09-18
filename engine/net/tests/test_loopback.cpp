// SPDX-License-Identifier: GPL-3.0-or-later
// The in-memory link, driven with opaque byte strings.
//
// No session and no messages here: this file is about the channel, and a channel that only
// works on well-formed input is not a channel. The session that runs over it is proved
// separately, which is what keeps a failure in either one legible.
#include <atlas/core/assert.hpp>
#include <atlas/net/loopback.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using atlas::net::CommandInbox;
using atlas::net::LinkFault;
using atlas::net::LoopbackHub;

namespace {

const bool kMainThreadMarkedForLoopback = [] {
    atlas::mark_main_thread();
    return true;
}();

/// A message long enough that a corrupting flip lands past the header, tagged so a test can say
/// which one arrived.
[[nodiscard]] std::vector<std::byte> tagged(std::uint8_t tag) {
    std::vector<std::byte> message(32, std::byte{0});
    message[20] = static_cast<std::byte>(tag);
    return message;
}

[[nodiscard]] std::vector<std::uint8_t> drain_tags(CommandInbox& inbox) {
    std::vector<std::vector<std::byte>> messages;
    inbox.drain(messages);
    std::vector<std::uint8_t> tags;
    tags.reserve(messages.size());
    for (const auto& message : messages) {
        tags.push_back(std::to_integer<std::uint8_t>(message[20]));
    }
    return tags;
}

}  // namespace

TEST_CASE("a link needs two ends", "[net][loopback]") {
    CHECK_FALSE(LoopbackHub::create({.peer_count = 1}).has_value());
    CHECK_FALSE(LoopbackHub::create({.peer_count = 0}).has_value());
    CHECK(LoopbackHub::create({.peer_count = 2}).has_value());
}

TEST_CASE("a message sent with no latency arrives on the next poll", "[net][loopback]") {
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    auto sender = (*hub)->end(0);
    auto receiver = (*hub)->end(1);

    REQUIRE(sender.send_to(1, tagged(7)).has_value());
    // Not before the poll. Delivery is something the receiver does, which is what makes a
    // stalled peer able to receive at all.
    CHECK(receiver.inbox(0).depth() == 0);

    receiver.pump();
    CHECK(drain_tags(receiver.inbox(0)) == std::vector<std::uint8_t>{7});
    CHECK((*hub)->stats().delivered == 1);
}

TEST_CASE("latency is counted in the receiver's polls", "[net][loopback]") {
    // The property the whole design rests on. If this were counted in ticks, a peer waiting on
    // a turn would never advance its tick, so the turn would never arrive, so it would wait for
    // ever — a deadlock in exactly the situation the gate exists for.
    auto hub = LoopbackHub::create({.peer_count = 2, .latency_polls = 2});
    REQUIRE(hub.has_value());
    auto sender = (*hub)->end(0);
    auto receiver = (*hub)->end(1);

    REQUIRE(sender.send_to(1, tagged(3)).has_value());

    receiver.pump();
    CHECK(receiver.inbox(0).depth() == 0);
    receiver.pump();
    CHECK(receiver.inbox(0).depth() == 0);
    receiver.pump();
    CHECK(drain_tags(receiver.inbox(0)) == std::vector<std::uint8_t>{3});

    // And the sender polling changes nothing, which is half of what "the receiver's polls"
    // means.
    REQUIRE(sender.send_to(1, tagged(4)).has_value());
    for (int i = 0; i < 10; ++i) {
        sender.pump();
    }
    CHECK(receiver.inbox(0).depth() == 0);
    // A latency of two means the third poll after the send, as the configuration documents.
    receiver.pump();
    receiver.pump();
    CHECK(receiver.inbox(0).depth() == 0);
    receiver.pump();
    CHECK(drain_tags(receiver.inbox(0)) == std::vector<std::uint8_t>{4});

    // The other half, and the half a first version of this case missed. Latency is counted from
    // where the receiver is *now*, not from some absolute start — so a message sent to a peer
    // that has already polled a hundred times still takes two more of its polls. Measuring
    // against the sender's count instead gives the same answer only while both ends happen to
    // be level, which they are at the top of this case and never are in a real session.
    for (int i = 0; i < 100; ++i) {
        receiver.pump();
    }
    REQUIRE(sender.send_to(1, tagged(5)).has_value());
    receiver.pump();
    receiver.pump();
    CHECK(receiver.inbox(0).depth() == 0);
    receiver.pump();
    CHECK(drain_tags(receiver.inbox(0)) == std::vector<std::uint8_t>{5});
}

TEST_CASE("one seed delivers one order, and another seed delivers another", "[net][loopback]") {
    const auto transcript = [](std::uint64_t seed, bool reorder) {
        auto hub = LoopbackHub::create(
            {.peer_count = 2, .latency_polls = 0, .reorder = reorder, .reorder_seed = seed});
        REQUIRE(hub.has_value());
        auto sender = (*hub)->end(0);
        auto receiver = (*hub)->end(1);
        for (std::uint8_t i = 0; i < 16; ++i) {
            REQUIRE(sender.send_to(1, tagged(i)).has_value());
        }
        receiver.pump();
        return drain_tags(receiver.inbox(0));
    };

    // Reproducible: the same seed twice is the same delivery order, which is what lets a
    // failing lockstep run be re-run.
    CHECK(transcript(7, true) == transcript(7, true));
    // And not vacuous: a different seed gives a different order, and reordering off gives the
    // order they were sent in. Without these two, the case above passes on a link that never
    // reorders anything.
    CHECK(transcript(7, true) != transcript(99, true));
    std::vector<std::uint8_t> in_order;
    in_order.reserve(16);
    for (std::uint8_t i = 0; i < 16; ++i) {
        in_order.push_back(i);
    }
    CHECK(transcript(7, false) == in_order);
    CHECK(transcript(7, true) != in_order);
}

TEST_CASE("reordering permutes without losing or duplicating", "[net][loopback]") {
    auto hub = LoopbackHub::create(
        {.peer_count = 2, .latency_polls = 0, .reorder = true, .reorder_seed = 5});
    REQUIRE(hub.has_value());
    auto sender = (*hub)->end(0);
    auto receiver = (*hub)->end(1);
    for (std::uint8_t i = 0; i < 32; ++i) {
        REQUIRE(sender.send_to(1, tagged(i)).has_value());
    }
    receiver.pump();

    auto arrived = drain_tags(receiver.inbox(0));
    REQUIRE(arrived.size() == 32);
    std::ranges::sort(arrived);
    for (std::uint8_t i = 0; i < 32; ++i) {
        CHECK(arrived[i] == i);
    }
}

TEST_CASE("a dropped message never arrives, and the link says one was dropped", "[net][loopback]") {
    // The count matters as much as the absence. A fault that never fired would leave the
    // message missing for the wrong reason, and every assertion about a stall would pass on a
    // link that did nothing.
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    auto sender = (*hub)->end(0);
    auto receiver = (*hub)->end(1);

    REQUIRE((*hub)->arm_fault(0, 1, LinkFault::Drop).has_value());
    REQUIRE(sender.send_to(1, tagged(1)).has_value());
    receiver.pump();
    CHECK(receiver.inbox(0).depth() == 0);
    CHECK((*hub)->stats().dropped == 1);

    // One shot: the next message goes through.
    REQUIRE(sender.send_to(1, tagged(2)).has_value());
    receiver.pump();
    CHECK(drain_tags(receiver.inbox(0)) == std::vector<std::uint8_t>{2});
}

TEST_CASE("a held message waits until it is released", "[net][loopback]") {
    // What makes "stalls rather than diverges" testable without a clock: the message is parked
    // rather than lost, and releasing it finishes the session that was waiting.
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    auto sender = (*hub)->end(0);
    auto receiver = (*hub)->end(1);

    REQUIRE((*hub)->arm_fault(0, 1, LinkFault::Hold).has_value());
    REQUIRE(sender.send_to(1, tagged(9)).has_value());
    for (int i = 0; i < 20; ++i) {
        receiver.pump();
    }
    CHECK(receiver.inbox(0).depth() == 0);
    // Parked rather than absent, which is the distinction a test of a stall needs to make.
    CHECK((*hub)->held_count() == 1);
    CHECK((*hub)->stats().held == 1);

    (*hub)->release_held();
    CHECK((*hub)->held_count() == 0);
    receiver.pump();
    CHECK(drain_tags(receiver.inbox(0)) == std::vector<std::uint8_t>{9});
}

TEST_CASE("a corrupted message arrives, changed, past its header", "[net][loopback]") {
    // Past the header on purpose. A flip inside the magic would be refused before anything
    // interesting happened, which would test the magic check rather than the session.
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    auto sender = (*hub)->end(0);
    auto receiver = (*hub)->end(1);

    const auto original = tagged(0x10);
    REQUIRE((*hub)->arm_fault(0, 1, LinkFault::Corrupt).has_value());
    REQUIRE(sender.send_to(1, original).has_value());
    receiver.pump();

    std::vector<std::vector<std::byte>> messages;
    receiver.inbox(0).drain(messages);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].size() == original.size());
    CHECK(messages[0] != original);
    CHECK((*hub)->stats().corrupted == 1);
    // The first sixteen bytes are intact, so it still claims to be a message of a known type.
    for (std::size_t i = 0; i < 16; ++i) {
        INFO("byte " << i);
        CHECK(messages[0][i] == original[i]);
    }
}

TEST_CASE("one peer's backlog is in its own mailbox", "[net][loopback]") {
    // One inbox per sender, so a peer sending faster than it can be read fills its own and
    // starves nobody else's. A single shared queue would let one participant's flood push out
    // another's turn, and a lost turn is a tick that never runs.
    auto hub = LoopbackHub::create({.peer_count = 3});
    REQUIRE(hub.has_value());
    auto loud = (*hub)->end(0);
    auto quiet = (*hub)->end(1);
    auto receiver = (*hub)->end(2);

    for (std::size_t i = 0; i < CommandInbox::kMaxMessages + 50; ++i) {
        REQUIRE(loud.send_to(2, tagged(1)).has_value());
    }
    REQUIRE(quiet.send_to(2, tagged(2)).has_value());
    receiver.pump();

    CHECK(receiver.inbox(0).overflowed());
    // The quiet peer's turn is untouched by the loud peer's flood.
    CHECK_FALSE(receiver.inbox(1).overflowed());
    CHECK(drain_tags(receiver.inbox(1)) == std::vector<std::uint8_t>{2});
}

TEST_CASE("a peer does not send to itself", "[net][loopback]") {
    // It has its own turn already and needs no link to learn of it. Refused rather than
    // silently ignored, because a session that sent to itself would be marking its own gate
    // twice and would look fine until it did not.
    auto hub = LoopbackHub::create({.peer_count = 2});
    REQUIRE(hub.has_value());
    auto peer = (*hub)->end(0);
    CHECK_FALSE(peer.send_to(0, tagged(1)).has_value());
    CHECK_FALSE(peer.send_to(9, tagged(1)).has_value());
}

TEST_CASE("a broadcast reaches every peer but the sender", "[net][loopback]") {
    auto hub = LoopbackHub::create({.peer_count = 3});
    REQUIRE(hub.has_value());
    auto sender = (*hub)->end(1);
    REQUIRE(sender.broadcast(tagged(5)).has_value());

    for (const std::size_t peer : {std::size_t{0}, std::size_t{2}}) {
        auto receiver = (*hub)->end(peer);
        receiver.pump();
        INFO("peer " << peer);
        CHECK(drain_tags(receiver.inbox(1)) == std::vector<std::uint8_t>{5});
    }
    CHECK((*hub)->stats().sent == 2);
}

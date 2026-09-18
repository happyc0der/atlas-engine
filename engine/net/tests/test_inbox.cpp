// SPDX-License-Identifier: GPL-3.0-or-later
// The inbox, on one thread.
//
// What is being pinned is that it refuses rather than evicts, and that the refusal is
// permanent. A ring buffer that dropped the oldest message would pass a naive test and lose a
// turn, and a lost turn is a tick that runs on every other peer and not on this one.
#include <atlas/core/assert.hpp>
#include <atlas/net/inbox.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using atlas::net::CommandInbox;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// A message whose first byte says which one it is, so an eviction is visible as a gap.
[[nodiscard]] std::vector<std::byte> numbered(std::size_t index, std::size_t bytes = 8) {
    std::vector<std::byte> message(bytes, std::byte{0});
    message[0] = static_cast<std::byte>(index & 0xFFU);
    return message;
}

}  // namespace

TEST_CASE("a full inbox refuses rather than evicts", "[net][inbox]") {
    CommandInbox inbox;
    for (std::size_t i = 0; i < CommandInbox::kMaxMessages; ++i) {
        REQUIRE(inbox.push(numbered(i)) == CommandInbox::Push::Accepted);
    }
    CHECK(inbox.depth() == CommandInbox::kMaxMessages);
    CHECK_FALSE(inbox.overflowed());

    CHECK(inbox.push(numbered(999)) == CommandInbox::Push::Overflowed);
    CHECK(inbox.overflowed());

    // The messages that were accepted are all there, in order, and the one refused is not among
    // them. A ring that evicted the oldest would hold the same number and a different set.
    std::vector<std::vector<std::byte>> drained;
    inbox.drain(drained);
    REQUIRE(drained.size() == CommandInbox::kMaxMessages);
    for (std::size_t i = 0; i < drained.size(); ++i) {
        INFO("message " << i);
        CHECK(drained[i][0] == static_cast<std::byte>(i & 0xFFU));
    }
}

TEST_CASE("an overflowed inbox stays overflowed", "[net][inbox]") {
    // Draining makes room; it does not make the refused messages come back. Resuming after a
    // gap produces a stream that looks complete and is not, and the divergence it causes would
    // be attributed to whichever system touched the missing command's table.
    CommandInbox inbox;
    for (std::size_t i = 0; i < CommandInbox::kMaxMessages + 1; ++i) {
        (void)inbox.push(numbered(i));
    }
    REQUIRE(inbox.overflowed());

    std::vector<std::vector<std::byte>> drained;
    inbox.drain(drained);
    CHECK(inbox.depth() == 0);
    CHECK(inbox.overflowed());

    CHECK(inbox.push(numbered(0)) == CommandInbox::Push::Overflowed);
    inbox.drain(drained);
    CHECK(drained.empty());
}

TEST_CASE("the byte budget trips before the message count does", "[net][inbox]") {
    // Two bounds rather than one refined. A message count alone bounds memory at "256 times
    // whatever a message may be", and a turn may legitimately carry thousands of commands.
    //
    // Bounded, because an inbox that evicted instead of refusing would accept for ever and this
    // loop would spin rather than fail. Mutation testing found that: the defect showed up as a
    // test consuming a core indefinitely, which in continuous integration is a timeout with no
    // message attached. A bound turns it back into a failure that says what is wrong.
    constexpr std::size_t kLarge = CommandInbox::kMaxBytes / 8;
    constexpr std::size_t kFarPastAnyLimit = CommandInbox::kMaxMessages * 4;
    CommandInbox inbox;
    std::size_t accepted = 0;
    while (accepted < kFarPastAnyLimit &&
           inbox.push(numbered(accepted, kLarge)) == CommandInbox::Push::Accepted) {
        ++accepted;
    }
    REQUIRE(accepted < kFarPastAnyLimit);
    CHECK(inbox.overflowed());
    // Eight of these fill the byte budget, far short of the message count.
    CHECK(accepted == 8);
    CHECK(accepted < CommandInbox::kMaxMessages);
    CHECK(inbox.queued_bytes() <= CommandInbox::kMaxBytes);
}

TEST_CASE("draining hands over everything and leaves nothing", "[net][inbox]") {
    CommandInbox inbox;
    for (std::size_t i = 0; i < 5; ++i) {
        REQUIRE(inbox.push(numbered(i)) == CommandInbox::Push::Accepted);
    }

    std::vector<std::vector<std::byte>> drained;
    // Deliberately not empty beforehand: the buffer is reused once a frame, and one that was
    // appended to rather than cleared would report every message it had ever seen.
    drained.emplace_back(4, std::byte{0xFF});
    inbox.drain(drained);
    REQUIRE(drained.size() == 5);
    CHECK(drained[0][0] == std::byte{0});

    CHECK(inbox.depth() == 0);
    CHECK(inbox.queued_bytes() == 0);
    CHECK(inbox.accepted() == 5);
    CHECK(inbox.refused() == 0);

    inbox.drain(drained);
    CHECK(drained.empty());
}

TEST_CASE("an empty drain is not an early return", "[net][inbox]") {
    // The asset registry's pump learned this the hard way: it returned early on an empty
    // completion list, and a stall is exactly the case where nothing has arrived. Draining
    // nothing must still be a call that happened.
    CommandInbox inbox;
    std::vector<std::vector<std::byte>> drained;
    drained.emplace_back(1, std::byte{0xAB});
    inbox.drain(drained);
    CHECK(drained.empty());
}

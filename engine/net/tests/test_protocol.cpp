// SPDX-License-Identifier: GPL-3.0-or-later
// The protocol's constants, pinned.
//
// A test over constants looks like a test of nothing. It is a test that changing one of these
// is a decision rather than an edit: a version bump refuses every existing peer, and a limit
// the encoder and the decoder disagree about is how the replay format came to be able to write
// files it could not read.
#include <atlas/net/protocol.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/replay.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the protocol version is three", "[net][protocol]") {
    // Bumped in M25 by ADR-0022, for the drop message and the relay's framing. Two was M21's,
    // for the finish message; one was M14's.
    STATIC_REQUIRE(atlas::net::kProtocolVersion == 3);
}

TEST_CASE("the magic is not any other format's magic", "[net][protocol]") {
    // A save handed to a socket, or a message handed to a loader, must fail immediately and say
    // which it was rather than being half-parsed as the other.
    STATIC_REQUIRE(atlas::net::kNetMagic != atlas::sim::kReplayMagic);
}

TEST_CASE("one command fits in one message", "[net][protocol]") {
    // The two limits are set in different files for different reasons, and a legal command that
    // cannot be sent would be a hole nothing else here would notice.
    STATIC_REQUIRE(atlas::net::kMaxMessageBytes > atlas::sim::CommandQueue::kMaxPayload);
}

TEST_CASE("no message type is zero", "[net][protocol]") {
    // So that a zero-filled buffer is a violation rather than a valid message.
    STATIC_REQUIRE(static_cast<std::uint32_t>(atlas::net::MessageType::Hello) != 0);
    STATIC_REQUIRE(static_cast<std::uint32_t>(atlas::net::ByeReason::Quit) != 0);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What two peers agree on before they agree on anything else: versions, limits, and the set of
/// messages that exist.
///
/// The model and the reasoning are
/// [ADR-0014](../../../../../docs/adr/0014-deterministic-lockstep.md). In one sentence: a tick runs
/// only when every participant has said what it is doing on that tick, so every peer applies the
/// same commands in the same order and reaches the same state without exchanging any state at all.
///
/// **There is no transport here and none is planned in this milestone.** What this module holds
/// is the shape a transport would plug into, plus an in-memory link that exercises it.
///
/// **Every limit is declared here, and the encoder checks the same number the decoder
/// enforces.** That is not tidiness. The replay format had its ceilings in one translation unit
/// where only the reader could see them, and the writer could therefore produce a recording
/// that was written successfully and could never be read — fixed in this milestone's opening
/// sweep, and not repeated here.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace atlas::net {

/// Bumped when the meaning of any message changes. A peer speaking a different version is
/// refused at the handshake rather than half-understood, exactly as a save or a replay is.
///
/// Version 2 (M21) added `Finish`: a session can end because its run is over rather than only
/// because something failed (ADR-0020). A version 1 peer would read a finish as a type it does
/// not have, which is a violation, so the two cannot be mixed and the handshake says so first.
inline constexpr std::uint32_t kProtocolVersion = 2;

/// Leading bytes of every message. Distinct from the save and replay magics, so a file handed
/// to a socket, or a message handed to a loader, fails immediately and says which it was.
inline constexpr std::uint64_t kNetMagic = 0x54'45'4E'53'41'4C'54'41ULL;  // "ATLASNET"

/// What a message is.
///
/// Zero is deliberately unused, so a zero-filled buffer is a violation rather than a valid
/// message. An unknown value is a violation too, never a skip: a protocol that ignores what it
/// does not understand cannot be versioned later without silently changing meaning.
enum class MessageType : std::uint32_t {
    Hello = 1,
    Welcome = 2,
    Turn = 3,
    HashCheck = 4,
    Bye = 5,
    /// This peer will run no tick after the one named (ADR-0020). Added in version 2.
    Finish = 6,
};

/// Why a session ended. Zero is unused for the same reason as above.
enum class ByeReason : std::uint32_t {
    /// The peer chose to leave.
    Quit = 1,
    /// The peer sent something this build cannot make sense of.
    ProtocolError = 2,
    /// The state hashes stopped matching. Atlas does not resync (ADR-0014).
    Diverged = 3,
    /// The peer sent faster than it could be read.
    Overflow = 4,
    /// The handshake found a version or a golden hash that does not match.
    VersionMismatch = 5,
};

/// The name of a farewell reason, for a log line a person reads.
[[nodiscard]] std::string_view to_string(ByeReason reason) noexcept;

/// Most peers one session will hold.
///
/// Lockstep runs at the pace of the slowest participant, so the practical ceiling is far below
/// this. The number exists to bound what a handshake may claim, since a peer list arrives from
/// whatever assembled the session and is untrusted like any other input.
inline constexpr std::size_t kMaxPeers = 16;

/// Most commands one turn may carry.
///
/// A turn is one peer's input for one tick. Four thousand is orders of magnitude past what a
/// person can produce and comfortably past what a mod should.
inline constexpr std::size_t kMaxCommandsPerTurn = 4096;

/// Most per-system hashes one hash check may carry. Matches the replay's own ceiling, because
/// the two carry the same thing and a divergence should read the same either way.
inline constexpr std::size_t kMaxSystemHashesPerCheck = 4096;

/// Most bytes one encoded message may occupy.
///
/// Checked when a message is written as well as when it is read, so the cap is what makes
/// "a peer cannot make me allocate arbitrarily" true rather than hoped for.
///
/// **This number is not free to choose: it must fit in an empty `CommandInbox`.** Until M17 it
/// was eight mebibytes against an inbox budget of one, so every message between the two was
/// legal to encode, legal to send, and impossible to receive — it tripped the inbox's sticky,
/// session-fatal overflow at the far end. The loopback never built messages that large, so
/// nothing covered it, and it would have become reachable on the first real socket.
/// `inbox.hpp` carries the assertion that ties the two together; this comment and that one are
/// one rule written twice on purpose, because the numbers live in different files.
///
/// **What it does *not* equal is the largest turn the other ceilings permit**, and saying so
/// matters. A command costs thirty-two bytes of framing plus its payload, so four thousand of
/// them at the queue's 64 KiB payload ceiling would be about 256 MiB — two orders of magnitude
/// past this. **So the binding constraint on a turn is this cap and not
/// `kMaxCommandsPerTurn`**, and a producer that fills the command count with large payloads is
/// refused by the encoder rather than by the counter it would expect. For scale in the other
/// direction: four thousand commands with the lab's five-byte payloads encode to about 148 KiB,
/// which is a seventh of this.
inline constexpr std::size_t kMaxMessageBytes = std::size_t{1024} * 1024;

/// Longest build identifier a handshake may carry, and longest human-readable detail a farewell
/// may carry. Neither is parsed; both are for a person reading a log.
inline constexpr std::size_t kMaxBuildIdBytes = 256;
inline constexpr std::size_t kMaxByeDetailBytes = 512;

}  // namespace atlas::net

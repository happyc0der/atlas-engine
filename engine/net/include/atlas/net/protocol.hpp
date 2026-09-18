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

namespace atlas::net {

/// Bumped when the meaning of any message changes. A peer speaking a different version is
/// refused at the handshake rather than half-understood, exactly as a save or a replay is.
inline constexpr std::uint32_t kProtocolVersion = 1;

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
/// Checked when a message is written as well as when it is read. A turn of four thousand
/// commands at the queue's 64 KiB payload limit would be far larger than this; the cap is what
/// makes "a peer cannot make me allocate arbitrarily" true rather than hoped for.
inline constexpr std::size_t kMaxMessageBytes = std::size_t{8} * 1024 * 1024;

/// Longest build identifier a handshake may carry, and longest human-readable detail a farewell
/// may carry. Neither is parsed; both are for a person reading a log.
inline constexpr std::size_t kMaxBuildIdBytes = 256;
inline constexpr std::size_t kMaxByeDetailBytes = 512;

}  // namespace atlas::net

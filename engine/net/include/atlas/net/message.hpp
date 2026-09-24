// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The five messages two peers exchange, and the bytes they are.
///
/// **There is no framing here, deliberately.** Every message is a standalone buffer produced by
/// one writer and consumed by one reader that ends at `expect_end`. The in-memory link moves
/// whole buffers; a transport would add its own framing, because a length prefix that suits a
/// stream socket is wrong for a datagram and vice versa. This module owns the message, not the
/// frame.
///
/// **Every encoder refuses what its decoder would refuse, before writing anything.** That rule
/// is not general tidiness: the replay format had its ceilings where only the reader could see
/// them, so a long enough recording was written successfully and could never be read again.
/// M14's opening sweep fixed that, and this is the same milestone declining to reintroduce it
/// one file away. Every count is checked against the constant in `protocol.hpp` that the
/// decoder uses, and the finished buffer is checked against the message size cap.
///
/// **An unknown message type is a violation, never a skip.** A protocol that ignores what it
/// does not understand cannot be versioned later without silently changing meaning, and a peer
/// that can make another peer ignore a message can make it ignore a turn.
///
/// Thread affinity: none. These are pure functions over buffers.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/net/protocol.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/kernel.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace atlas::net {

/// What a peer says when it arrives, and everything needed to refuse it.
///
/// The golden hashes are the compatibility probe. Two builds that produce the same values for
/// the fixed scenario agree about the simulation whatever else differs between them, which is
/// the condition the charter puts on a session between different builds. A differing build
/// identifier is logged rather than refused, so that a debug peer and a release peer whose
/// goldens agree can still play — which is a thing worth being able to test.
struct Hello {
    std::uint32_t hash_algorithm_version = 0;
    std::uint32_t save_format_version = 0;
    std::uint32_t replay_format_version = 0;
    std::string build_id;
    std::uint64_t golden_final_state = 0;
    std::uint64_t golden_all_ticks = 0;
    std::uint64_t seed = 0;
    Tick start_tick = 0;
    std::uint64_t initial_state_hash = 0;
    std::uint32_t tick_rate = 0;
    /// Ticks between stamping a command and running it. The session agrees on the largest
    /// proposal, because a peer given less delay than it needs is late every tick.
    std::uint32_t proposed_delay = 0;
};

/// What a peer is told in reply: who it is, who else is playing, and what was agreed.
struct Welcome {
    /// The identifier this peer must stamp its commands with. `SourceId::Local` means peer
    /// zero and not "me", which is why it is carried rather than assumed.
    sim::SourceId assigned_source = sim::SourceId::Local;
    /// Every participant, **strictly increasing**. Refused otherwise on both sides: this list
    /// becomes the gate's expectation set, and a repeated identifier makes the gate wait for one
    /// peer twice and never become ready.
    std::vector<sim::SourceId> peers;
    std::uint64_t agreed_seed = 0;
    Tick start_tick = 0;
    std::uint64_t initial_state_hash = 0;
    std::uint32_t agreed_delay = 0;
};

/// One peer's entire input for one tick.
///
/// **One message rather than commands plus a separate completion**, and that is a correctness
/// decision rather than a saving. Split in two, the gate would depend on transport ordering: a
/// completion arriving before its commands opens the gate on an incomplete turn, the tick runs,
/// the commands arrive late, and the peers have diverged with nothing lost and no error raised.
/// A correct-looking run with different state is the worst failure available here. One message
/// makes "arrived" and "complete" the same event, which no reordering can separate.
///
/// **An empty turn is sent explicitly**, because silence is also what a dead peer produces.
struct Turn {
    Tick tick = 0;
    sim::SourceId source = sim::SourceId::Local;
    std::vector<sim::Command> commands;
};

/// What one peer made of a tick, for comparing against what another did.
struct HashCheck {
    Tick tick = 0;
    sim::SourceId source = sim::SourceId::Local;
    std::uint64_t state_hash = 0;
    /// Per-system, so a mismatch can be attributed rather than merely detected. Same shape as
    /// the replay's checkpoint, so a divergence between peers reads like a divergence in a
    /// recording.
    std::vector<sim::SystemHash> system_hashes;
};

/// Why a session ended. `detail` is for a person reading a log and is never parsed.
struct Bye {
    ByeReason reason = ByeReason::Quit;
    std::string detail;
};

/// This peer will run no tick after `last_tick`, and needs nothing further after it (ADR-0020).
///
/// **Not a goodbye.** A peer that sends this is still there: it goes on delivering the turns
/// and hash checks its partners need up to `last_tick`, and goes on comparing theirs. A `Bye`
/// means the sender has gone and ends the receiver's session on arrival; making one of its
/// reasons mean "still here" would put a branch on the reason into every place a goodbye is
/// handled.
///
/// Every peer must finish at the same tick. Two that do not have disagreed about what the run
/// was, and that is a protocol violation.
struct Finish {
    Tick last_tick = 0;
};

using Message = std::variant<Hello, Welcome, Turn, HashCheck, Bye, Finish>;

/// The type tag of a message, without decoding the rest of it.
///
/// Fails for a buffer that is too short, has the wrong magic, speaks another protocol version,
/// or names a type this build does not have.
[[nodiscard]] Result<MessageType> peek_type(std::span<const std::byte> bytes);

/// Encode one message.
///
/// Fails with `Exhausted` when the message holds more than a decoder would accept — more peers,
/// more commands, more system hashes, a longer string, or a larger total — naming the limit.
/// Checked before the bytes are written where the count is known, so an oversized message is
/// refused without building it.
[[nodiscard]] Result<std::vector<std::byte>> encode(const Message& message);

/// Decode one message, which must occupy the whole buffer.
///
/// Fails with `MalformedData` for a bad magic, a truncated buffer, a count past its limit, an
/// unknown type or reason, a peer list that is not strictly increasing, or trailing bytes; and
/// with `VersionMismatch` for another protocol version.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> bytes);

}  // namespace atlas::net

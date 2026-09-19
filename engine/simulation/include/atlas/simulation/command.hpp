// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Commands: everything from outside that can change the simulation.
///
/// Input, a script, a network peer and a replay all reach the simulation the same way. That
/// is not tidiness: it is what makes a replay possible at all. If input could reach state by
/// any other route, recording commands would not capture everything that happened.
///
/// **Stamped, not immediate.** A command names the tick it applies to. It never takes effect
/// when it arrives, because arrival time depends on the frame rate, on the network, and on
/// which thread got there first, and none of those may influence results.
///
/// **Ordered by source and sequence, never by arrival.** At the start of a tick the commands
/// for it are sorted by `(source, sequence)`, a total order that two machines can agree on
/// without agreeing on timing.
///
/// **Payloads are bytes the kernel does not interpret.** A registered decoder validates each
/// one before it is applied. A command that fails validation is logged and dropped whole; it
/// is never partly applied, because half a command is a state no author ever reasoned about.
///
/// Thread affinity: the queue is main-thread only. A command arriving on another thread is
/// handed across before being submitted.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/simulation/world.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::sim {

/// Who issued a command.
///
/// An index, not an address: it is written to replays and compared across machines. Zero is
/// the local source.
enum class SourceId : std::uint32_t { Local = 0 };

/// The bit that separates a peer from a mod.
///
/// A peer's identifier is its index in the session — low, dense, and agreed at the handshake.
/// A mod's is local to the machine it runs on and is **never sent on the wire**: every peer
/// runs the same mods and each produces the same commands for itself, and the hash check is
/// what catches a mod that decided differently (ADR-0014, ADR-0015).
///
/// Split by a bit rather than by a range so the two can never be confused by arithmetic, and
/// so a source that arrives over a link can be recognised as impossible rather than merely
/// unexpected. ADR-0014 asserted this scheme; until M15 it was prose and nothing else.
inline constexpr std::uint32_t kModSourceBit = 0x8000'0000U;

/// The identifier of the `index`-th mod on this machine.
[[nodiscard]] constexpr SourceId mod_source(std::uint32_t index) noexcept {
    return SourceId{kModSourceBit | index};
}

/// Whether this identifier belongs to a mod rather than to a peer.
[[nodiscard]] constexpr bool is_mod_source(SourceId source) noexcept {
    return (static_cast<std::uint32_t>(source) & kModSourceBit) != 0;
}

/// What kind of command this is. The application assigns the meanings.
enum class CommandType : std::uint32_t { Invalid = 0 };

[[nodiscard]] constexpr CommandType command_type(std::string_view name) noexcept {
    const std::uint64_t full = hash_string(name);
    const auto folded = static_cast<std::uint32_t>((full >> 32U) ^ (full & 0xFFFF'FFFFULL));
    return CommandType{folded == 0 ? 1U : folded};
}

/// One command, with its payload held inline.
struct Command {
    Tick target = 0;
    SourceId source = SourceId::Local;

    /// Monotonic per source, and the second half of the order `drain` sorts by.
    ///
    /// `submit` assigns it and cannot repeat one. `submit_stamped` keeps what it is given,
    /// because reproducing a recording means reproducing its numbers — so for that path this
    /// is a promise made by the caller rather than a property of the queue, and an untrusted
    /// caller can break it. `drain` therefore breaks a tie on the command's own content, and
    /// `net::Session` refuses a turn whose commands are not all labelled with the peer that
    /// sent it. Neither is redundant: the first keeps the order defined, the second keeps a
    /// peer from reaching it.
    std::uint64_t sequence = 0;

    CommandType type = CommandType::Invalid;
    std::vector<std::byte> payload;
};

/// How a command type is validated and applied.
struct CommandHandler {
    /// Check the payload before anything is changed.
    ///
    /// Separate from apply so that a bad command is rejected whole. A validator that
    /// returned an error halfway through applying would leave the state partly changed,
    /// which is the one outcome worth ruling out entirely.
    std::function<Status(std::span<const std::byte>)> validate;

    /// Apply a payload that has already been validated.
    std::function<void(World&, std::span<const std::byte>)> apply;
};

/// Commands waiting for the ticks they name.
class CommandQueue {
  public:
    /// Most commands held for any single tick. A queue growing past this is a runaway
    /// producer, and failing loudly beats consuming memory until something else does.
    static constexpr std::size_t kMaxPending = 1'000'000;

    /// Largest payload accepted, since a queue is fed from untrusted places.
    static constexpr std::size_t kMaxPayload = std::size_t{64} * 1024;

    CommandQueue();
    ~CommandQueue();

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;
    CommandQueue(CommandQueue&& other) noexcept;
    CommandQueue& operator=(CommandQueue&& other) noexcept;

    /// Register how a command type is validated and applied.
    [[nodiscard]] Status register_handler(CommandType type, CommandHandler handler);

    [[nodiscard]] bool has_handler(CommandType type) const noexcept;

    /// Queue a command for a future tick.
    ///
    /// Fails for an unregistered type, an oversized payload, a full queue, or a payload the
    /// registered validator rejects. Validating on submission rather than on execution means
    /// the caller hears about its own mistake while it still has the context to fix it.
    ///
    /// The sequence number is assigned by the queue, per source, so a caller cannot
    /// accidentally reuse one and make the order ambiguous.
    [[nodiscard]] Status submit(Tick target, SourceId source, CommandType type,
                                std::span<const std::byte> payload);

    /// Queue a command that already carries its stamp, as a replay does.
    ///
    /// Keeps the recorded sequence number rather than assigning a new one, because the order
    /// in the recording is the thing being reproduced.
    [[nodiscard]] Status submit_stamped(Command command);

    /// Take the commands for `tick`, in `(source, sequence)` order.
    ///
    /// Removes them from the queue. Commands stamped for ticks already past are returned
    /// too, because dropping them silently would make a late command disappear with no
    /// record; the kernel decides what to do about them.
    [[nodiscard]] std::vector<Command> drain(Tick tick);

    /// Apply one command through its registered handler.
    ///
    /// Fails if the type has no handler or the payload no longer validates.
    [[nodiscard]] Status apply(World& world, const Command& command) const;

    [[nodiscard]] std::size_t pending() const noexcept { return m_pending.size(); }

    /// The next sequence number a source will be given, for saving and restoring.
    [[nodiscard]] std::uint64_t next_sequence(SourceId source) const noexcept;
    void set_next_sequence(SourceId source, std::uint64_t sequence);

    /// Every source that has issued a command, in identifier order.
    ///
    /// Ordered so that a save file is canonical: iterating whatever the container happened
    /// to hold would make the bytes depend on insertion order.
    [[nodiscard]] std::vector<SourceId> sources() const;

    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::vector<Command> m_pending;  ///< Unordered; drain sorts what it takes.
};

}  // namespace atlas::sim

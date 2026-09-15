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

/// What kind of command this is. The application assigns the meanings.
enum class CommandType : std::uint32_t { Invalid = 0 };

[[nodiscard]] constexpr CommandType command_type(std::string_view name) noexcept {
    const std::uint64_t full = hash_string(name);
    const auto folded = static_cast<std::uint32_t>((full >> 32) ^ (full & 0xFFFF'FFFFULL));
    return CommandType{folded == 0 ? 1U : folded};
}

/// One command, with its payload held inline.
struct Command {
    Tick target = 0;
    SourceId source = SourceId::Local;

    /// Monotonic per source. Two commands from one source never share a sequence number, so
    /// `(source, sequence)` is a total order.
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
    static constexpr std::size_t kMaxPayload = 64 * 1024;

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

    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::vector<Command> m_pending;  ///< Unordered; drain sorts what it takes.
};

}  // namespace atlas::sim

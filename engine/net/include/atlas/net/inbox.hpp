// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Where messages from one peer wait for the main thread.
///
/// **One inbox per remote peer**, so that a peer sending faster than it can be read fills its
/// own and starves nobody else's. A single shared queue would let one participant's flood push
/// out another's turn, and a lost turn is a tick that never runs.
///
/// **Bounded, and overflow is fatal to the session.** The two precedents in this repository both
/// decline to bound: the snapshot channel keeps one entry and drops the rest, which is right for
/// presentation and catastrophic here; the asset registry's completion list is unbounded, which
/// is right when the producer is a pool whose depth you control and wrong when the producer is a
/// peer. So this follows the second shape — a mutex, a vector, drained by swap on the main
/// thread — and adds the bound the second shape lacks.
///
/// **Once overflowed it accepts nothing further.** It does not recover, because a lockstep
/// stream with a hole in it is worthless: the commands that fell out were going to be applied on
/// every other peer, so resuming afterwards produces a divergence that would be attributed to
/// whichever system happened to touch the missing command's table. Failing loudly at the gap is
/// the only honest option.
///
/// Thread affinity: **`push` from any thread, everything else from the main thread.** A message
/// arriving on a transport's own thread is handed across here and submitted to the command queue
/// later, which is what the command queue's "main thread only" contract has always required.

#include <atlas/core/result.hpp>
#include <atlas/net/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace atlas::net {

/// A bounded, many-producer, single-consumer mailbox for one peer's messages.
class CommandInbox {
  public:
    /// Most messages one inbox will hold before it refuses.
    ///
    /// At sixty ticks a second this is over four seconds of backlog, which is far more than a
    /// lockstep session can be behind and still be a session: the gate stops the simulation
    /// after the input delay is exhausted, so a peer this far ahead is not playing the same
    /// game. Overflow therefore means what it should mean.
    static constexpr std::size_t kMaxMessages = 256;

    /// And most bytes, which is a separate bound rather than a refinement.
    ///
    /// A message count alone bounds memory at "256 times whatever a message may be", and a turn
    /// may legitimately carry thousands of commands. Either limit alone is not a limit.
    ///
    /// **This is a total across every message waiting, so a single message this size overflows
    /// an empty inbox.** That is what makes the relationship with `kMaxMessageBytes` load
    /// bearing rather than incidental, and it is asserted below rather than left to two
    /// comments agreeing by luck.
    static constexpr std::size_t kMaxBytes = std::size_t{1024} * 1024;

    /// The largest message the encoder will produce must fit in an empty inbox.
    ///
    /// **A bound that cannot admit a legal message is not a bound; it is a bug with a limit's
    /// name on it.** Before M17 these two numbers disagreed by a factor of eight and nothing
    /// said so, because they live in different headers and each looked reasonable alone. This
    /// is the check that makes the next disagreement a build failure instead of a session that
    /// ends on a peer's first large turn.
    static_assert(kMaxMessageBytes <= kMaxBytes,
                  "a message the encoder accepts must fit in an empty inbox");

    enum class Push : std::uint8_t {
        Accepted,
        /// Refused and discarded, and every later push will be too.
        Overflowed,
    };

    CommandInbox() = default;

    CommandInbox(const CommandInbox&) = delete;
    CommandInbox& operator=(const CommandInbox&) = delete;
    CommandInbox(CommandInbox&&) = delete;
    CommandInbox& operator=(CommandInbox&&) = delete;

    ~CommandInbox() = default;

    /// Hand a message over. Any thread.
    ///
    /// Never blocks on anything but the mutex, which is held for a move and a size check.
    [[nodiscard]] Push push(std::vector<std::byte> message);

    /// Take everything waiting. Main thread only.
    ///
    /// Swaps rather than copies, so the lock is held for a pointer exchange rather than for the
    /// length of whatever the caller then does with the messages. `out` is cleared first.
    void drain(std::vector<std::vector<std::byte>>& out);

    /// Whether this inbox has refused a message and is therefore no longer a complete record of
    /// what its peer sent. Sticky.
    [[nodiscard]] bool overflowed() const noexcept;

    /// Messages waiting to be drained.
    [[nodiscard]] std::size_t depth() const noexcept;

    /// Bytes waiting to be drained.
    [[nodiscard]] std::size_t queued_bytes() const noexcept;

    /// Messages accepted over this inbox's lifetime, and messages refused.
    [[nodiscard]] std::uint64_t accepted() const noexcept;
    [[nodiscard]] std::uint64_t refused() const noexcept;

  private:
    mutable std::mutex m_mutex;
    std::vector<std::vector<std::byte>> m_pending;
    std::size_t m_queued_bytes = 0;
    bool m_overflowed = false;
    std::uint64_t m_accepted = 0;
    std::uint64_t m_refused = 0;
};

}  // namespace atlas::net

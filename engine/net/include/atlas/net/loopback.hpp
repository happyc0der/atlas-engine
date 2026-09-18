// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Several peers in one process, connected by a link that is unkind on purpose.
///
/// There is no transport in this milestone and none is planned in it. What there is instead is
/// this: an in-memory hub that can delay, reorder, drop, corrupt and withhold messages on
/// demand, so that the lockstep session is proved against a channel that misbehaves rather than
/// against one that cannot.
///
/// **Latency is counted in receiver polls, never in ticks**, and that is the single most
/// important sentence in the file. A tick runs only when the gate is ready, and the gate is
/// ready only when every expected turn has arrived. Condition delivery on the receiver's *tick*
/// advancing and, in exactly the situation the gate exists for — A waiting on B — A's tick does
/// not advance, so B's turn is never delivered, so A waits for ever. Tick-based latency
/// deadlocks precisely when the gate is doing its job, which would make this link latency-free
/// in every case anybody cares about and a hang in the rest. A poll is something a stalled peer
/// still does.
///
/// **Reordering is reproducible from a seed.** It is driven by the counter-based random stream
/// the simulation already uses, keyed on the receiver's poll index, so the permutation is a pure
/// function of the seed and the poll and does not depend on how many messages any other pair
/// happened to carry. A stateful shuffle would reintroduce exactly the property that engine
/// avoids: adding one call anywhere shifting every value everywhere.
///
/// **Faults are armed rather than scheduled**, one shot, on the next message over one ordered
/// pair. This link knows nothing about ticks — it moves bytes — so when a fault fires is the
/// caller's business, which is also what lets a test place one precisely.
///
/// Thread affinity: the main thread throughout. This is a test and demonstration vehicle, and
/// the thing that would run on another thread — a real transport — is what does not exist yet.
/// The inboxes it fills are safe to push into from anywhere, which is where that property will
/// be needed.

#include <atlas/core/result.hpp>
#include <atlas/net/inbox.hpp>
#include <atlas/net/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace atlas::net {

/// What one injected fault does to one message.
enum class LinkFault : std::uint8_t {
    /// Never delivered. Must produce a stall rather than a divergence.
    Drop,
    /// Delivered with one bit flipped, past the header so that it lands in the body rather than
    /// being rejected as "not an Atlas message" before anything interesting happens.
    Corrupt,
    /// Parked until `release_held`, which is what makes "stalls rather than diverges" testable
    /// without waiting on a clock.
    Hold,
};

struct LoopbackConfig {
    std::size_t peer_count = 2;
    /// Delay in **receiver polls**. See the file comment; this is not ticks, and it must not be.
    ///
    /// A latency of L means a message is delivered on the receiver's (L + 1)th poll after it
    /// was sent: zero is "the next poll", one is "the one after that", and so on. Counted from
    /// where the receiver is when the message is sent, not from any absolute start, so a peer
    /// that has already polled a thousand times still waits L more.
    std::uint32_t latency_polls = 0;
    /// Permute the messages released in one poll.
    bool reorder = false;
    std::uint64_t reorder_seed = 0;
};

/// What the link did, for a test that needs to know a fault actually fired.
struct LinkStats {
    std::uint64_t sent = 0;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t corrupted = 0;
    std::uint64_t held = 0;
    /// Messages released in an order other than the one they were sent in.
    std::uint64_t reordered = 0;
    /// Messages an inbox refused. Non-zero means a session should be ending.
    std::uint64_t refused = 0;
};

class LoopbackHub;

/// One peer's end of the link.
class LinkEnd {
  public:
    /// Send to one other peer. Fails for a peer index this hub does not have, or for sending to
    /// oneself — a peer that needs its own turn has it already and does not need the network to
    /// tell it.
    [[nodiscard]] Status send_to(std::size_t peer, std::span<const std::byte> message);

    /// Send to every peer but this one.
    [[nodiscard]] Status broadcast(std::span<const std::byte> message);

    /// Advance this peer's poll counter and deliver whatever is now due.
    ///
    /// Called once per frame whether or not a tick ran, which is what makes poll-based latency
    /// work: a stalled peer still polls, so the turn it is waiting for still arrives.
    void pump();

    /// This peer's mailbox for messages from `peer`. One per sender, so a peer sending faster
    /// than it can be read fills its own and starves nobody else's.
    [[nodiscard]] CommandInbox& inbox(std::size_t peer);

    [[nodiscard]] std::size_t index() const noexcept { return m_index; }

    [[nodiscard]] std::size_t peer_count() const noexcept;
    /// Polls this end has made, which is what latency is measured in.
    [[nodiscard]] std::uint64_t polls() const noexcept;

  private:
    friend class LoopbackHub;

    LinkEnd(LoopbackHub& hub, std::size_t index) noexcept : m_hub(&hub), m_index(index) {}

    LoopbackHub* m_hub;
    std::size_t m_index;
};

/// Every peer's link, and the messages in flight between them.
class LoopbackHub {
  public:
    /// Fails for fewer than two peers — a link with one end is not a link — or for more than
    /// `kMaxPeers`.
    [[nodiscard]] static Result<std::unique_ptr<LoopbackHub>> create(const LoopbackConfig& config);

    LoopbackHub(const LoopbackHub&) = delete;
    LoopbackHub& operator=(const LoopbackHub&) = delete;
    LoopbackHub(LoopbackHub&&) = delete;
    LoopbackHub& operator=(LoopbackHub&&) = delete;
    ~LoopbackHub() = default;

    [[nodiscard]] LinkEnd end(std::size_t peer);

    [[nodiscard]] std::size_t peer_count() const noexcept { return m_peers.size(); }

    /// Arm a one-shot fault on the next message sent from `from` to `to`.
    ///
    /// Replaces any fault already armed on that pair. Fails for an index this hub does not have.
    [[nodiscard]] Status arm_fault(std::size_t from, std::size_t to, LinkFault fault);

    /// Release every message parked by a `Hold`, so they deliver on the next poll.
    void release_held() noexcept;

    /// Messages currently parked by a hold, so a test can prove one is actually parked rather
    /// than merely absent.
    [[nodiscard]] std::size_t held_count() const noexcept;

    [[nodiscard]] const LinkStats& stats() const noexcept { return m_stats; }

  private:
    friend class LinkEnd;

    struct Pending {
        std::vector<std::byte> bytes;
        std::uint64_t release_at_poll = 0;
        std::uint64_t order = 0;
        bool held = false;
    };

    /// One ordered pair's state: what is in flight, and what fault is armed on it.
    struct Pipe {
        std::deque<Pending> in_flight;
        std::optional<LinkFault> armed;
    };

    struct Peer {
        /// One inbox per sender, indexed by that sender's peer index. The entry for this peer's
        /// own index exists and is never used, which costs a pointer and removes an index
        /// adjustment from every lookup.
        std::vector<std::unique_ptr<CommandInbox>> inboxes;
        std::uint64_t polls = 0;
    };

    explicit LoopbackHub(const LoopbackConfig& config);

    [[nodiscard]] Pipe& pipe(std::size_t from, std::size_t to);

    [[nodiscard]] Status send(std::size_t from, std::size_t to, std::span<const std::byte> message);
    void pump(std::size_t peer);

    LoopbackConfig m_config;
    std::vector<Peer> m_peers;
    /// Row-major by (from, to), so a pair is one multiplication away.
    std::vector<Pipe> m_pipes;
    std::uint64_t m_next_order = 0;
    LinkStats m_stats;
};

}  // namespace atlas::net

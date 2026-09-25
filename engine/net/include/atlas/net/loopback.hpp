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
#include <atlas/net/link.hpp>
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
    /// A mesh by default. A star routes as the socket hub does since M25 (ADR-0022 D9): peer
    /// zero files every broadcast it receives and forwards it to everyone else in the same step,
    /// and no other pair has a direct connection. What lets the drop agreement be tested under
    /// this link's latency, reordering and holds, in one process.
    Topology topology = Topology::Mesh;
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

/// Every peer's link, and the messages in flight between them.
class LoopbackHub final : public Link {
  public:
    /// Fails for fewer than two peers — a link with one end is not a link — or for more than
    /// `kMaxPeers`.
    [[nodiscard]] static Result<std::unique_ptr<LoopbackHub>> create(const LoopbackConfig& config);

    LoopbackHub(const LoopbackHub&) = delete;
    LoopbackHub& operator=(const LoopbackHub&) = delete;
    LoopbackHub(LoopbackHub&&) = delete;
    LoopbackHub& operator=(LoopbackHub&&) = delete;
    ~LoopbackHub() override = default;

    [[nodiscard]] std::size_t peer_count() const noexcept override { return m_peers.size(); }

    /// Move one message, subject to whatever this link has been told to do to it.
    [[nodiscard]] Status send(std::size_t from, std::size_t to,
                              std::span<const std::byte> message) override;

    /// Send to every other peer: a loop over `send` on a mesh, and on a star one message to
    /// peer zero, which forwards it when it arrives.
    [[nodiscard]] Status broadcast(std::size_t from, std::span<const std::byte> message) override;

    [[nodiscard]] Topology topology() const noexcept override { return m_config.topology; }

    [[nodiscard]] bool lost(std::size_t peer) const noexcept override;

    /// Lose one peer, as a process that died would be lost: what it had sent and was still in
    /// flight never arrives, anything it sends from now on vanishes, and nothing reaches it.
    ///
    /// **What a relay has already forwarded is not recalled.** On a star, a message of the lost
    /// peer's that peer zero received was filed and forwarded in the same step, and the forwarded
    /// copies stay in flight — exactly as packets the listener has already queued would.
    ///
    /// Fails for an index this hub does not have.
    [[nodiscard]] Status lose(std::size_t peer);

    /// Advance one peer's poll counter and deliver whatever is now due.
    void pump(std::size_t peer) override;

    [[nodiscard]] CommandInbox& inbox(std::size_t peer, std::size_t from) override;

    /// Arm a one-shot fault on the next message sent from `from` to `to`.
    ///
    /// Replaces any fault already armed on that pair. Fails for an index this hub does not have.
    [[nodiscard]] Status arm_fault(std::size_t from, std::size_t to, LinkFault fault);

    /// Release every message parked by a `Hold`, so they deliver on the next poll.
    void release_held() noexcept;

    /// Messages currently parked by a hold, so a test can prove one is actually parked rather
    /// than merely absent.
    [[nodiscard]] std::size_t held_count() const noexcept;

    /// Polls one end has made, which is what this link measures latency in.
    ///
    /// **Moved off `LinkEnd` in M17, where it did not belong.** A poll count is how the
    /// in-memory link decides a message is due; a socket has no such notion, and `LinkEnd` is
    /// about to become the type every transport implements. Leaving it there would have handed
    /// the socket implementation a method with no meaning and a comment describing the other
    /// one. It had no callers, which is the only reason this is a move rather than a migration.
    ///
    /// Returns zero for an index this hub does not have.
    [[nodiscard]] std::uint64_t polls(std::size_t peer) const noexcept;

    [[nodiscard]] const LinkStats& stats() const noexcept { return m_stats; }

  private:
    struct Pending {
        std::vector<std::byte> bytes;
        std::uint64_t release_at_poll = 0;
        std::uint64_t order = 0;
        bool held = false;
        /// Whose message this is. The sender, except for one peer zero forwarded on a star.
        std::size_t origin = 0;
        /// On a star, a broadcast on its way to peer zero, to be forwarded when it arrives.
        bool everyone = false;
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

    /// Put one message in flight on one pipe, applying whatever fault is armed on it.
    void enqueue(std::size_t from, std::size_t to, std::vector<std::byte> bytes, std::size_t origin,
                 bool everyone);

    LoopbackConfig m_config;
    std::vector<Peer> m_peers;
    /// Row-major by (from, to), so a pair is one multiplication away.
    std::vector<Pipe> m_pipes;
    /// Peers this hub has been told to lose, by index.
    std::vector<bool> m_lost;
    std::uint64_t m_next_order = 0;
    LinkStats m_stats;
};

}  // namespace atlas::net

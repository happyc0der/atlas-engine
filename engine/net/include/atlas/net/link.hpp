// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What a session needs from whatever carries its messages (ADR-0017 D6).
///
/// **`LinkEnd` is a handle, not a connection.** It is one peer's index plus a pointer to the
/// thing that moves bytes, so copying one is cheap and keeping one is meaningless once that
/// thing is gone. `Session` takes it by value and always has; what must outlive the session is
/// the `Link` behind it.
///
/// **The interface was four calls until M25, and each addition has a reason.** It began as
/// exactly what `LinkEnd` already asked of `LoopbackHub` before M17 gave the asking a name, with
/// `broadcast` a loop over `send` on `LinkEnd`. ADR-0022 found that a socket session could not
/// have a third peer, because the socket hub is a star and a connector reaches only the
/// listener. The fix is a relay, and a relay needs to know that a message is for everyone rather
/// than for one peer — so `broadcast` became a call a backend may implement, with the loop as
/// its default. `topology` says which kind of link this is, because whether a lost peer can be
/// dropped at an agreed tick depends on it.
///
/// **What is not here is as decided as what is.** No connect, no disconnect, no address, no
/// notion of a peer arriving or leaving. A session is handed a link with its participants
/// already fixed, because ADR-0014 fixed the expectation set before the first tick and nothing
/// in lockstep re-opens it. How a link came to have peers is the business of whatever built it.
///
/// Thread affinity: **main thread**, for every call here. The inboxes a link fills are safe to
/// push into from any thread, which is where a transport with a receive thread would need it —
/// ADR-0017 D3 chose a polled transport, so nothing needs it today.

#include <atlas/core/result.hpp>
#include <atlas/net/inbox.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace atlas::net {

class LinkEnd;

/// How messages travel between peers (ADR-0022).
enum class Topology : std::uint8_t {
    /// Every peer reaches every other directly. The loopback's default.
    Mesh,
    /// Every peer reaches the others through peer zero, which forwards what it receives in the
    /// order it received it. The socket hub, whose connectors hold a connection to the listener
    /// and to nobody else.
    Star,
};

/// What a session does when a peer is lost (ADR-0022 D4).
enum class PeerLoss : std::uint8_t {
    /// End the session, as a divergence or a late turn does (ADR-0017 D5). The default.
    End,
    /// Peer zero drops the lost peer, every other peer checks it holds the same turns of it that
    /// peer zero does, and the session goes on without it. Needs a star: the agreement rests on
    /// every message reaching the others through peer zero.
    Drop,
};

/// Whatever moves messages between peers: an in-memory hub, or a socket.
class Link {
  public:
    virtual ~Link() = default;

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    Link(Link&&) = delete;
    Link& operator=(Link&&) = delete;

    /// How many peers this link connects, including the local one.
    [[nodiscard]] virtual std::size_t peer_count() const noexcept = 0;

    /// Send one whole message from one peer to another.
    ///
    /// **Whole, never partial.** There is no framing in this module by decision — a length
    /// prefix that suits a stream socket is wrong for a datagram — so a link either delivers a
    /// message entire or does not deliver it. Fails for an index this link does not have, and
    /// for sending to oneself.
    [[nodiscard]] virtual Status send(std::size_t from, std::size_t to,
                                      std::span<const std::byte> message) = 0;

    /// Send one whole message from one peer to every other.
    ///
    /// The default is a loop over `send`, which is right for a mesh. A star overrides it,
    /// because there a peer other than zero cannot reach the others except by asking peer zero
    /// to forward — and peer zero can only do that if it knows the message is for everyone.
    [[nodiscard]] virtual Status broadcast(std::size_t from, std::span<const std::byte> message);

    /// Which kind of link this is. See `Topology`.
    [[nodiscard]] virtual Topology topology() const noexcept = 0;

    /// Whether this link has given up on `peer`: nothing further from it will be delivered, and
    /// nothing sent to it will arrive.
    ///
    /// **Only a link that was asked to report losses rather than end on them says so** — a
    /// socket hub configured with `PeerLoss::Drop`, or a loopback told to lose a peer. Every
    /// other link ends outright, or never loses anybody, and answers false. What the session
    /// does about a lost peer is its own policy; the link only says what happened.
    [[nodiscard]] virtual bool lost(std::size_t peer) const noexcept {
        (void)peer;
        return false;
    }

    /// Give `peer` whatever has arrived for it.
    ///
    /// Called once a frame whether or not a tick ran. That is what lets a stalled peer keep
    /// receiving — and a stalled peer is the situation the turn gate exists for, so a link that
    /// only delivered when the simulation advanced would deadlock exactly when it mattered.
    virtual void pump(std::size_t peer) = 0;

    /// `peer`'s mailbox for messages from `from`.
    ///
    /// One per sender, so a peer sending faster than it can be read fills its own and starves
    /// nobody else's.
    [[nodiscard]] virtual CommandInbox& inbox(std::size_t peer, std::size_t from) = 0;

    /// A handle to one peer's end of this link.
    ///
    /// Non-virtual and defined once, so an implementation gets it by existing rather than by
    /// remembering to provide it. This is also why `LinkEnd`'s constructor needs exactly one
    /// friend rather than one per backend.
    [[nodiscard]] LinkEnd end(std::size_t peer);

  protected:
    Link() = default;
};

/// One peer's end of a link.
class LinkEnd {
  public:
    /// Send to one other peer. Fails for a peer index this link does not have, or for sending to
    /// oneself — a peer that needs its own turn has it already and does not need the network to
    /// tell it.
    [[nodiscard]] Status send_to(std::size_t peer, std::span<const std::byte> message);

    /// Send to every peer but this one.
    [[nodiscard]] Status broadcast(std::span<const std::byte> message);

    /// Deliver whatever is now due to this end.
    void pump();

    /// This peer's mailbox for messages from `peer`.
    [[nodiscard]] CommandInbox& inbox(std::size_t peer);

    [[nodiscard]] std::size_t index() const noexcept { return m_index; }

    [[nodiscard]] std::size_t peer_count() const noexcept;

    /// The link's topology. See `Link::topology`.
    [[nodiscard]] Topology topology() const noexcept;

    /// Whether the link has given up on `peer`. See `Link::lost`.
    [[nodiscard]] bool lost(std::size_t peer) const noexcept;

  private:
    friend class Link;

    LinkEnd(Link& link, std::size_t index) noexcept : m_link(&link), m_index(index) {}

    Link* m_link;
    std::size_t m_index;
};

}  // namespace atlas::net

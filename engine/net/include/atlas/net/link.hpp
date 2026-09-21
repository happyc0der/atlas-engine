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
/// **The interface is four calls, and that is deliberate.** It is exactly what `LinkEnd` already
/// asked of `LoopbackHub` before M17 gave the asking a name — no more, because every method here
/// is one a socket has to mean something by. `broadcast` is not among them: it is a loop over
/// `send`, so it stays on `LinkEnd` where one implementation serves every backend.
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
#include <span>

namespace atlas::net {

class LinkEnd;

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

  private:
    friend class Link;

    LinkEnd(Link& link, std::size_t index) noexcept : m_link(&link), m_index(index) {}

    Link* m_link;
    std::size_t m_index;
};

}  // namespace atlas::net

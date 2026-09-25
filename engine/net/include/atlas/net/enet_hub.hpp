// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A lockstep session over real sockets (ADR-0017).
///
/// **No ENet type appears here.** The library is an implementation detail of `enet_hub.cpp`,
/// which is what keeps the choice reversible: the hub is one implementation of `net::Link`, and
/// replacing it would touch one class rather than every caller. The same property keeps
/// ADR-0015's fallback runtime real.
///
/// **Polled, with no thread of its own** (ADR-0017 D3). `pump` drains whatever has arrived
/// during the poll that already happens once a frame. The threading table in
/// `docs/ARCHITECTURE.md` gains no row for this, and ARCHITECTURE's "No new thread" sentence
/// survives the milestone unedited.
///
/// **A peer that goes quiet ends the session** (ADR-0017 D5). Dropping it and continuing is
/// simulation-visible: every remaining peer would have to apply the drop at the identical tick
/// or diverge, which needs an agreement protocol of its own. The deadline lives here rather
/// than in the turn gate, so `sim::TurnGate` still reads no clock.
///
/// Thread affinity: **main thread**, except `EnetHub::connect`, which says why at its own
/// declaration.

#include <atlas/core/result.hpp>
#include <atlas/net/link.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::net {

/// Whatever process-wide state the socket library needs, owned rather than hidden.
///
/// ENet's `enet_initialize` calls `WSAStartup` on Windows and must be paired with
/// `enet_deinitialize`. That is process-wide state, and CLAUDE.md permits one such object —
/// the log sink registry — with `script::Runtime` recorded by ADR-0015 as the second. This is
/// the third, and it is an explicit object for the reason ADR-0017 D10 gives: a
/// reference-counted static inside the hub would make the initialisation invisible at every
/// call site and tie Winsock's lifetime to whichever hub happened to be destroyed last.
///
/// Created once by the composition root. **At most one may exist**, and a second while one is
/// alive asserts rather than failing, because two is a programmer error and not a condition to
/// recover from.
class EnetRuntime {
  public:
    [[nodiscard]] static Result<EnetRuntime> create();

    ~EnetRuntime();
    EnetRuntime(const EnetRuntime&) = delete;
    EnetRuntime& operator=(const EnetRuntime&) = delete;
    EnetRuntime(EnetRuntime&& other) noexcept;
    EnetRuntime& operator=(EnetRuntime&& other) noexcept;

    /// Whether one currently exists. For assertions and for tests that must not leave one
    /// behind; not for deciding whether to make one.
    [[nodiscard]] static bool alive() noexcept;

  private:
    EnetRuntime() = default;

    bool m_owns = false;
};

/// How a hub is set up, and what it treats as a peer having gone.
struct EnetConfig {
    /// How long a peer may say nothing before the session ends.
    ///
    /// **This is the only deadline in the whole design, and it lives here rather than in the
    /// turn gate.** ADR-0014's invariant is that readiness depends on who has reported and
    /// never on elapsed time; a timeout in the gate would break it. A transport noticing that a
    /// socket has gone quiet and ending the session does not, because the simulation still only
    /// ever asks who has reported.
    ///
    /// Ten seconds is long enough to survive a stall that is not a failure and short enough
    /// that nobody waits for a peer that is never coming back.
    std::chrono::milliseconds peer_timeout{10'000};

    /// Largest message this hub will accept from a peer, matching the protocol's own bound so
    /// an oversized packet is refused at the socket rather than deeper in.
    std::size_t max_message_bytes = kMaxMessageBytes;
};

/// The result of a `pump`, for a caller that wants to know why a session stopped.
struct EnetStatus {
    /// A peer that has gone, by index. Empty in the ordinary case.
    std::vector<std::size_t> closed;
    /// Set once a peer has gone quiet or disconnected, after which this hub delivers nothing.
    bool ended = false;
    /// Why, for a person reading a log. Empty while running.
    std::string reason;
};

/// Several peers over real sockets, connected by address.
///
/// **Direct address only** (ADR-0017 D4): connect by host and port. No encryption, no traversal,
/// no lobby, each deferred with a trigger in `docs/DEFERRED.md`.
///
/// **A star, and the listener relays** (ADR-0022 D1). A connector holds one connection, to the
/// listener. What a connector broadcasts goes to the listener once, marked as for everyone; the
/// listener files it for itself and forwards it to every other connector in the same step, on
/// the same reliable ordered channel it uses for its own messages, marked with where it came
/// from. So the listener holds everything it has forwarded, and every connector receives the
/// listener's own messages and the forwarded ones in the order the listener produced them —
/// the two properties a drop at an agreed tick rests on. Until M25 there was no relay, and a
/// session of three failed before its first tick.
///
/// **The listener is trusted.** It can forge any connector's messages, because it relays them.
/// Nothing here authenticates anybody; ADR-0017 D4 deferred that, and ADR-0022 records the
/// widening a third peer brings.
class EnetHub final : public Link {
  public:
    /// Bind a port and prepare for `expected_peers`, without waiting for any of them.
    ///
    /// **Binding and waiting are two calls on purpose.** A caller asking for port zero cannot
    /// learn which port it got until the socket exists, and it needs to know — to print it, or
    /// to hand it to the other side. One call that bound and then blocked would make the number
    /// available only after the thing that needed it had already finished waiting.
    ///
    /// The listener is peer zero. `port` may be zero, meaning the operating system chooses;
    /// `port()` then reports what it chose.
    [[nodiscard]] static Result<std::unique_ptr<EnetHub>> listen(const EnetRuntime& runtime,
                                                                 std::uint16_t port,
                                                                 std::size_t expected_peers,
                                                                 const EnetConfig& config = {});

    /// Wait until every expected peer has connected and been told its index.
    ///
    /// The listener assigns indices in connection order, because somebody must and both ends
    /// need to agree before any `SourceId` is stamped — a `SourceId` reaches the replay and the
    /// hash, so it cannot be provisional.
    ///
    /// **Nobody is told until everybody has arrived.** A connector cannot send until it knows
    /// its index, and a message sent before the last connector joined would be forwarded to
    /// fewer peers than the session has. Holding every index back until the set is complete
    /// makes that impossible rather than handled. The index message also carries the session's
    /// size, so a connector knows how many peers there are rather than inferring it from its
    /// own index — which it did until M25, and which made the second of three connectors
    /// believe it was in a session of two.
    ///
    /// This is the one place this class waits: a session cannot begin without its participants,
    /// and there is nothing to poll on behalf of yet. Fails if the deadline passes first,
    /// saying how many arrived.
    [[nodiscard]] Status accept(std::chrono::milliseconds timeout);

    /// Connect to a listener and learn which index it assigned.
    ///
    /// **Callable from any thread**, unlike everything that touches a live hub. It opens a
    /// socket and hands back something nobody else holds, which is a handoff rather than a
    /// race — the same reason an asset importer may decode on a worker.
    [[nodiscard]] static Result<std::unique_ptr<EnetHub>>
    connect(const EnetRuntime& runtime, std::string_view host, std::uint16_t port,
            std::chrono::milliseconds timeout, const EnetConfig& config = {});

    ~EnetHub() override;
    EnetHub(const EnetHub&) = delete;
    EnetHub& operator=(const EnetHub&) = delete;
    EnetHub(EnetHub&&) = delete;
    EnetHub& operator=(EnetHub&&) = delete;

    [[nodiscard]] std::size_t peer_count() const noexcept override;
    /// Send to one peer. A connector may send only to the listener: it has no connection to
    /// anybody else, and reaches them only by `broadcast`, through the relay.
    [[nodiscard]] Status send(std::size_t from, std::size_t to,
                              std::span<const std::byte> message) override;

    /// Send to every peer. From a connector this is one packet to the listener, which forwards
    /// it; from the listener it is one packet to each connector.
    [[nodiscard]] Status broadcast(std::size_t from, std::span<const std::byte> message) override;

    /// Always a star.
    [[nodiscard]] Topology topology() const noexcept override { return Topology::Star; }

    void pump(std::size_t peer) override;
    [[nodiscard]] CommandInbox& inbox(std::size_t peer, std::size_t from) override;

    /// This hub's own index, assigned by the listener.
    [[nodiscard]] std::size_t local_index() const noexcept;

    /// The port actually bound, which is what a caller asking for zero needs to know.
    ///
    /// **Asking for port zero is how the tests avoid colliding**: the operating system picks a
    /// free one, so two cases running at once cannot fight over a constant.
    [[nodiscard]] std::uint16_t port() const noexcept;

    [[nodiscard]] const EnetStatus& status() const noexcept;

  private:
    EnetHub();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::net

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Opening a lockstep session over a socket, the same way in every application that plays one.
///
/// Lifted out of the lab in M22, when chess became the second application to need it. What is
/// here is the part both do identically — read `HOST:PORT`, bind or connect, print the port a
/// harness waits for, and wait for the handshake with a deadline — and none of the loop around
/// it, which differs in every ingredient that matters (ADR-0018 D5).
///
/// A separate library from `app_common` because it links `atlas::net`, and an application that
/// plays no session should not link a socket library to get a camera controller.
///
/// Thread affinity: main thread, like the hub and the session it creates.

#include <atlas/core/result.hpp>
#include <atlas/net/enet_hub.hpp>
#include <atlas/net/session.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace atlas::app {

/// Where to find the other side: a port to listen on, or a host and port to connect to.
struct SocketEndpoint {
    bool listen = false;
    std::string host;
    /// 0 with `listen` lets the operating system choose, which a harness wants.
    std::uint16_t port = 0;
    /// Peers in the session, this one included. Meaningful only when listening.
    std::size_t expected_peers = 2;
};

/// Read `HOST:PORT`. The host may itself contain colons — an IPv6 literal — so the port is what
/// follows the last one. Fails on a missing colon, an empty host, and a port outside 1..65535.
[[nodiscard]] Result<SocketEndpoint> parse_connect(std::string_view host_port);

/// Bind or connect, and wait until every peer is there.
///
/// A listener prints `listening on port N` and flushes it **before** waiting, because a harness
/// that asked for port zero reads that line to learn where to send the other process — so it
/// has to appear while this one is still waiting rather than once it has given up.
///
/// `config` is handed to the hub unchanged; its default ends the session on a lost peer.
[[nodiscard]] Result<std::unique_ptr<net::EnetHub>>
open_socket_hub(const net::EnetRuntime& runtime, const SocketEndpoint& endpoint,
                std::chrono::milliseconds timeout, const net::EnetConfig& config = {});

/// Poll until the session has agreed, the link ends, or `timeout` passes.
///
/// Bounded in wall time rather than in polls: over a socket a poll can do nothing at all while
/// the other process is still starting up, and counting those would fail a session that was
/// merely waiting for a slower machine.
[[nodiscard]] Status await_handshake(net::Session& session, const net::EnetHub& hub,
                                     sim::CommandQueue& queue, sim::TurnGate& gate,
                                     std::chrono::milliseconds timeout);

}  // namespace atlas::app

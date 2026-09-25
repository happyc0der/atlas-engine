// SPDX-License-Identifier: GPL-3.0-or-later
// The socket transport, over real loopback sockets.
//
// These are the first tests in this repository to open a socket. They run everywhere by
// decision (ADR-0017), with **the operating system choosing the port** -- `listen` is asked for
// zero and `port()` reports what it got -- so two cases running at once cannot collide on a
// constant. Every wait has a deadline measured in seconds, because a networking test that hangs
// blocks a continuous-integration job until the harness kills it.
//
// What is deliberately not here is a session. Two kernels agreeing hash for hash over a socket
// is M17's whole-program proof and belongs with the other whole-program proofs; this file is
// about the transport underneath behaving when things go wrong.
#include <atlas/core/assert.hpp>
#include <atlas/net/enet_hub.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using atlas::net::CommandInbox;
using atlas::net::EnetHub;
using atlas::net::EnetRuntime;
using namespace std::chrono_literals;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// One runtime for the whole file.
///
/// At most one may exist in a process (ADR-0017 D10), and Catch2 runs every case in one
/// process, so a per-case runtime would assert on the second. A function-local static gives
/// every case the same one without any case owning it.
[[nodiscard]] EnetRuntime& runtime() {
    static EnetRuntime instance = [] {
        auto made = EnetRuntime::create();
        if (!made) {
            std::abort();
        }
        return std::move(*made);
    }();
    return instance;
}

/// A listener and a connector on the loopback address, brought up together.
///
/// The connector runs on a thread because `accept` blocks -- a property of running both halves
/// in one process, which the lab does not have to do: there each side is its own process, which
/// is what slice 4 builds. Binding and accepting being two calls is what makes this possible at
/// all, because the port has to be readable before anybody can connect to it.
struct Pair {
    std::unique_ptr<EnetHub> listener;
    std::unique_ptr<EnetHub> connector;
};

[[nodiscard]] Pair connected_pair() {
    auto listener = EnetHub::listen(runtime(), 0, 2);
    REQUIRE(listener.has_value());
    const std::uint16_t port = (*listener)->port();
    REQUIRE(port != 0);

    auto joined = std::async(std::launch::async, [port] {
        // Its own thread, so it does not need this one to have reached `accept` yet: ENet
        // queues the connection request either way.
        return EnetHub::connect(runtime(), "127.0.0.1", port, 5s);
    });

    REQUIRE((*listener)->accept(5s).has_value());
    auto connector = joined.get();
    REQUIRE(connector.has_value());

    return {std::move(*listener), std::move(*connector)};
}

/// Pump both ends until `box` has something, or give up.
[[nodiscard]] bool wait_for_message(EnetHub& first, EnetHub& second, CommandInbox& box) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        first.pump(first.local_index());
        second.pump(second.local_index());
        if (box.depth() > 0) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::size_t count, std::byte fill = std::byte{9}) {
    return {count, fill};
}

}  // namespace

TEST_CASE("a runtime is alive once created", "[net][enet]") {
    REQUIRE(kMainThreadMarked);
    (void)runtime();
    CHECK(EnetRuntime::alive());
}

TEST_CASE("a listener binds a port the operating system chose", "[net][enet]") {
    // Asked for zero, so a non-zero answer means the kernel picked one. This is what lets cases
    // run concurrently without agreeing a constant, and what the integration harness needs.
    auto hub = EnetHub::listen(runtime(), 0, 2);
    REQUIRE(hub.has_value());
    CHECK((*hub)->port() != 0);
    CHECK((*hub)->local_index() == 0);
    CHECK((*hub)->peer_count() == 2);
}

TEST_CASE("a session of fewer than two or more than sixteen is refused", "[net][enet]") {
    CHECK_FALSE(EnetHub::listen(runtime(), 0, 0).has_value());
    CHECK_FALSE(EnetHub::listen(runtime(), 0, 1).has_value());
    CHECK_FALSE(EnetHub::listen(runtime(), 0, 17).has_value());
}

TEST_CASE("accepting nobody fails at the deadline rather than waiting", "[net][enet]") {
    auto hub = EnetHub::listen(runtime(), 0, 2);
    REQUIRE(hub.has_value());

    const auto before = std::chrono::steady_clock::now();
    const auto accepted = (*hub)->accept(200ms);
    const auto waited = std::chrono::steady_clock::now() - before;

    REQUIRE_FALSE(accepted.has_value());
    // The bound is the assertion. A listener that waited for ever is the failure mode this
    // whole file is written against.
    CHECK(waited < 3s);
}

TEST_CASE("connecting to nothing fails rather than hanging", "[net][enet]") {
    const auto before = std::chrono::steady_clock::now();
    const auto hub = EnetHub::connect(runtime(), "127.0.0.1", 1, 300ms);
    const auto waited = std::chrono::steady_clock::now() - before;

    REQUIRE_FALSE(hub.has_value());
    CHECK(waited < 3s);
}

TEST_CASE("an unresolvable host is refused", "[net][enet]") {
    CHECK_FALSE(EnetHub::connect(runtime(), "no-such-host.invalid", 9999, 300ms).has_value());
}

TEST_CASE("two peers connect and the listener assigns the indices", "[net][enet]") {
    auto pair = connected_pair();

    // The listener is peer zero by construction and the connector learns what it is from the
    // first message, before ADR-0014's own handshake begins. Nothing can stamp a SourceId until
    // this has happened, which is why it is not part of the protocol proper.
    CHECK(pair.listener->local_index() == 0);
    CHECK(pair.connector->local_index() == 1);
    CHECK(pair.listener->peer_count() == 2);
    CHECK(pair.connector->peer_count() == 2);
}

TEST_CASE("a message crosses a real socket whole", "[net][enet]") {
    auto pair = connected_pair();

    const auto sent = bytes_of(1000, std::byte{0x5A});
    REQUIRE(pair.listener->send(0, 1, sent).has_value());

    auto& box = pair.connector->inbox(1, 0);
    REQUIRE(wait_for_message(*pair.listener, *pair.connector, box));

    std::vector<std::vector<std::byte>> drained;
    box.drain(drained);
    REQUIRE(drained.size() == 1);

    // Whole, not merely present. There is no framing in this module by decision, so a transport
    // that delivered a message in pieces would break the codec rather than delay it -- which is
    // the second reason ADR-0017 chose a datagram library over a stream socket.
    CHECK(drained.front() == sent);
}

TEST_CASE("a hub sends only as itself, and never to itself", "[net][enet]") {
    auto pair = connected_pair();

    // The loopback can carry any pair because every peer is in one process. A socket hub cannot
    // even be asked: claiming to be another peer is refused rather than attempted.
    CHECK_FALSE(pair.listener->send(1, 0, bytes_of(4)).has_value());
    CHECK_FALSE(pair.listener->send(0, 0, bytes_of(4)).has_value());
    CHECK_FALSE(pair.listener->send(0, 9, bytes_of(4)).has_value());
}

TEST_CASE("a message past the protocol's bound is refused at the socket", "[net][enet]") {
    auto pair = connected_pair();

    // Refused here rather than deeper in, so an oversized packet never reaches an inbox that
    // would treat it as a session-fatal overflow. The bound is the protocol's own, which since
    // M17 slice 0 is the same number the inbox can hold.
    const auto oversized = bytes_of(atlas::net::kMaxMessageBytes + 1);
    const auto status = pair.listener->send(0, 1, oversized);
    REQUIRE_FALSE(status.has_value());
}

TEST_CASE("a peer that goes quiet without saying so ends the session too", "[net][enet]") {
    // The other half of the same policy, and it needs its own case because the mechanism
    // differs: a process killed outright never sends a disconnect, so nothing arrives and the
    // deadline is the only thing that notices. A graceful close and a hard kill are different
    // failures, and neither test covers the other.
    auto listener = EnetHub::listen(runtime(), 0, 2, {.peer_timeout = 150ms});
    REQUIRE(listener.has_value());
    const std::uint16_t port = (*listener)->port();

    auto joined = std::async(std::launch::async,
                             [port] { return EnetHub::connect(runtime(), "127.0.0.1", port, 5s); });
    REQUIRE((*listener)->accept(5s).has_value());
    const auto connector = joined.get();
    REQUIRE(connector.has_value());

    // Nobody says anything. The connector is alive and silent, which is what a hung peer looks
    // like from here and what a killed one looks like until its socket is reaped.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && !(*listener)->status().ended) {
        (*listener)->pump(0);
        std::this_thread::sleep_for(5ms);
    }

    REQUIRE((*listener)->status().ended);
    CHECK((*listener)->status().reason.contains("heard from"));
}

TEST_CASE("a peer that disconnects ends the session rather than being dropped", "[net][enet]") {
    auto pair = connected_pair();

    // ADR-0017 D5. Continuing without it is simulation-visible: every remaining peer would have
    // to apply the drop at the identical tick or diverge, and agreeing that tick is a protocol
    // this milestone deliberately does not build.
    pair.connector.reset();

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && !pair.listener->status().ended) {
        pair.listener->pump(0);
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(pair.listener->status().ended);
    CHECK_FALSE(pair.listener->status().reason.empty());
    REQUIRE(pair.listener->status().closed.size() == 1);
    CHECK(pair.listener->status().closed.front() == 1);

    // And it stays ended: sending into a finished session is refused rather than queued.
    CHECK_FALSE(pair.listener->send(0, 1, bytes_of(4)).has_value());
}

// ---------------------------------------------------------------------------------------------
// The relay (ADR-0022 D1). Until M25 a connector held a connection to the listener and nobody
// else, and a session of three failed before its first tick.

namespace {

struct Trio {
    std::unique_ptr<EnetHub> listener;
    std::unique_ptr<EnetHub> first;
    std::unique_ptr<EnetHub> second;
};

[[nodiscard]] Trio connected_trio(const atlas::net::EnetConfig& config = {}) {
    auto listener = EnetHub::listen(runtime(), 0, 3, config);
    REQUIRE(listener.has_value());
    const std::uint16_t port = (*listener)->port();

    // Both connect before the listener has told either its index: the listener holds every
    // index back until the set is complete, so both calls wait on the same accept.
    auto first = std::async(std::launch::async, [port, config] {
        return EnetHub::connect(runtime(), "127.0.0.1", port, 5s, config);
    });
    auto second = std::async(std::launch::async, [port, config] {
        return EnetHub::connect(runtime(), "127.0.0.1", port, 5s, config);
    });
    REQUIRE((*listener)->accept(5s).has_value());
    auto one = first.get();
    auto two = second.get();
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());

    // Connection order decides the indices, and two threads race to connect, so which future
    // got which index is not fixed. Sort them so the cases below can name peers by index.
    if ((*one)->local_index() > (*two)->local_index()) {
        std::swap(one, two);
    }
    return {std::move(*listener), std::move(*one), std::move(*two)};
}

/// Pump all three until every box has something, or give up.
[[nodiscard]] bool wait_for_all(Trio& trio, std::span<CommandInbox* const> boxes) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        trio.listener->pump(0);
        trio.first->pump(1);
        trio.second->pump(2);
        if (std::ranges::all_of(boxes, [](const CommandInbox* box) { return box->depth() > 0; })) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

[[nodiscard]] std::vector<std::byte> only_message(CommandInbox& box) {
    std::vector<std::vector<std::byte>> drained;
    box.drain(drained);
    REQUIRE(drained.size() == 1);
    return drained.front();
}

}  // namespace

TEST_CASE("three peers connect and every connector learns the session's size", "[net][enet]") {
    auto trio = connected_trio();

    // The second of three used to believe it was in a session of two, because it inferred the
    // size from its own index. Now the listener says.
    CHECK(trio.first->local_index() == 1);
    CHECK(trio.second->local_index() == 2);
    CHECK(trio.listener->peer_count() == 3);
    CHECK(trio.first->peer_count() == 3);
    CHECK(trio.second->peer_count() == 3);
    CHECK(trio.first->topology() == atlas::net::Topology::Star);
}

TEST_CASE("a connector's broadcast reaches every peer, through the listener", "[net][enet]") {
    auto trio = connected_trio();

    const auto sent = bytes_of(700, std::byte{0x3C});
    REQUIRE(trio.first->broadcast(1, sent).has_value());

    // Filed by the listener for itself, and forwarded to the other connector filed under the
    // peer it came from rather than the peer that delivered it.
    std::array<CommandInbox*, 2> boxes{&trio.listener->inbox(0, 1), &trio.second->inbox(2, 1)};
    REQUIRE(wait_for_all(trio, boxes));
    CHECK(only_message(*boxes[0]) == sent);
    CHECK(only_message(*boxes[1]) == sent);
    // And to nobody else: the connector that sent it does not receive its own message back.
    CHECK(trio.first->inbox(1, 2).depth() == 0);
    CHECK(trio.second->inbox(2, 0).depth() == 0);
}

TEST_CASE("the listener's broadcast reaches every connector", "[net][enet]") {
    auto trio = connected_trio();

    const auto sent = bytes_of(40, std::byte{0x11});
    REQUIRE(trio.listener->broadcast(0, sent).has_value());

    std::array<CommandInbox*, 2> boxes{&trio.first->inbox(1, 0), &trio.second->inbox(2, 0)};
    REQUIRE(wait_for_all(trio, boxes));
    CHECK(only_message(*boxes[0]) == sent);
    CHECK(only_message(*boxes[1]) == sent);
}

TEST_CASE("a connector reaches another connector only by broadcast", "[net][enet]") {
    auto trio = connected_trio();

    // Refused rather than forwarded. The listener holds everything it forwards only because it
    // forwards nothing but broadcasts, which it files for itself too (ADR-0022 D2).
    const auto refused = trio.first->send(1, 2, bytes_of(4));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("broadcast"));
    // To the listener alone is still a message, and is not forwarded.
    REQUIRE(trio.first->send(1, 0, bytes_of(4)).has_value());
    std::array<CommandInbox*, 1> boxes{&trio.listener->inbox(0, 1)};
    REQUIRE(wait_for_all(trio, boxes));
    for (int i = 0; i < 20; ++i) {
        trio.listener->pump(0);
        trio.second->pump(2);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(trio.second->inbox(2, 1).depth() == 0);
}

TEST_CASE("a quiet connector is noticed although another is still talking", "[net][enet]") {
    // One deadline per peer since M25. With one shared deadline, the listener heard from
    // somebody every few milliseconds and a silent third peer could never be noticed.
    auto trio = connected_trio({.peer_timeout = 200ms});

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && !trio.listener->status().ended) {
        // Peer 1 talks; peer 2 says nothing at all.
        REQUIRE(trio.first->broadcast(1, bytes_of(8)).has_value());
        trio.first->pump(1);
        trio.listener->pump(0);
        std::this_thread::sleep_for(5ms);
    }

    REQUIRE(trio.listener->status().ended);
    CHECK(trio.listener->status().reason.contains("peer 2"));
    CHECK(trio.listener->status().reason.contains("heard from"));
}

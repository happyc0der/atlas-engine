// SPDX-License-Identifier: GPL-3.0-or-later
//
// What lockstep bookkeeping costs per tick.
//
// Three scenarios:
//
//   net/gate_and_poll — every peer polls, sends its turn and marks the gate, and **does not
//                       step the kernel**. Stepping inside the measurement would measure the
//                       kernel — 11.8 ms of a 13.4 ms tick at a million cells, per
//                       PERFORMANCE.md — and report the network as free.
//   net/encode_turn   — the codec alone, writing.
//   net/decode_turn   — the codec alone, reading. Separate from encoding because decode is the
//                       one on the untrusted path, with every bound checked, and a change that
//                       speeds encoding by moving work into decoding should be visible as such.
//
// ---------------------------------------------------------------------------------------
// PREDICTION, written before the first run and committed before the numbers exist.
//
// `net/gate_and_poll` at two peers with eight commands per turn: **2 to 6 microseconds per peer
// per tick**, dominated by allocation rather than arithmetic. One turn is one writer buffer
// (one or two vector growths), one push under an uncontended mutex, one swap out, one reader
// pass, and eight calls to submit_stamped, each copying a small payload into a fresh vector —
// roughly twelve to twenty allocations per peer per tick. An uncontended mutex pair is tens of
// nanoseconds and should not appear at all.
//
// At sixty ticks a second and four peers that is under 0.15% of a 16.6 ms frame, so **the claim
// this benchmark is expected to support is that lockstep bookkeeping does not need optimising**,
// and the number exists to make that checkable rather than assumed. Above 50 µs the cause will
// be per-command allocation in `Command::payload`, and the fix would be a small-buffer payload —
// a `simulation` change, and outside M14.
//
// `net/decode_turn` will be **1.5 to 3 times** `net/encode_turn` at equal command count,
// because decoding allocates a vector per payload while encoding writes into one buffer.
//
// Being wrong here is fine and is the point of writing it down. Being unable to be wrong is not.
// ---------------------------------------------------------------------------------------
//
// M17 adds one more:
//
//   net/socket_roundtrip — one message from one process to another over the loopback network
//                          interface and back, through `EnetHub`. Two hubs in this process,
//                          which is not two machines: what it measures is the host's own
//                          send-and-receive path, not a network.
//
// ---------------------------------------------------------------------------------------
// PREDICTION for the socket scenario, written before the first run and committed before the
// numbers exist.
//
// **20 to 100 microseconds for a round trip.** A message goes through ENet's reliability
// bookkeeping, a `sendto` into the kernel, the loopback interface, a `recvfrom` in the other
// hub's `enet_host_service`, and the same again coming back. Two system calls each way is
// several microseconds before anything else happens, and ENet services its own queues on a
// timer granularity it controls.
//
// **The honest uncertainty is ENet's internal pacing, and it is large.** ENet batches outgoing
// packets and flushes on `enet_host_service`; with a zero timeout it flushes what is ready and
// returns, but whether a reply comes back on the next service call or the one after is a
// property of its scheduler rather than of this code. If the answer lands in the *milliseconds*
// the cause is that pacing, not the socket, and the fix would be `enet_host_flush` after every
// send — which this hub does not currently do outside the handshake.
//
// **The claim this is expected to support is narrow, and narrower than the other scenarios'.**
// Lockstep runs at the pace of the slowest participant and a tick is 16.6 ms at sixty a second,
// so even the pessimistic end of this range is under one per cent of a tick. What would matter
// is a round trip in the *tens* of milliseconds, because that would exceed a tick and turn the
// input delay into a stall. This number exists to notice that, not to be optimised.
//
// **What this deliberately does not measure is the network**, and no benchmark in this
// repository can. Two processes on one machine share a kernel and never leave it; latency
// between two real machines is a property of the wire, and the input-delay window exists
// precisely because that number is unknowable from here.
// ---------------------------------------------------------------------------------------

#include <atlas/core/assert.hpp>
#include <atlas/net/enet_hub.hpp>
#include <atlas/net/loopback.hpp>
#include <atlas/net/message.hpp>
#include <atlas/net/session.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include "harness.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace {

using atlas::bench::Result;

/// Abort rather than exit, which is what every other benchmark here does: a benchmark that
/// cannot set itself up has nothing to unwind, and a measurement taken after a failed setup
/// would be a number with no meaning.
[[noreturn]] void die(const std::string& why) {
    std::fprintf(stderr, "benchmark setup failed: %s\n", why.c_str());
    std::abort();
}

void require(const atlas::Status& status, std::string_view what) {
    if (!status) {
        die(std::format("{}: {}", what, status.error().message()));
    }
}

const atlas::sim::CommandType kPoke = atlas::sim::command_type("bench poke");

[[nodiscard]] atlas::sim::CommandHandler poke_handler() {
    atlas::sim::CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 5) {
            return std::unexpected(
                atlas::Error(atlas::ErrorCode::MalformedData, "expected five bytes"));
        }
        return atlas::ok();
    };
    handler.apply = [](atlas::sim::World&, const atlas::sim::ApplyContext&,
                       std::span<const std::byte>) { return atlas::ok(); };
    return handler;
}

[[nodiscard]] std::vector<atlas::sim::Command>
turn_of(std::size_t count, atlas::sim::SourceId source, atlas::Tick tick) {
    std::vector<atlas::sim::Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        commands.push_back(atlas::sim::Command{
            .target = tick,
            .source = source,
            .sequence = i,
            .type = kPoke,
            .payload = std::vector<std::byte>(5, std::byte{0x2A}),
        });
    }
    return commands;
}

/// One peer's session, queue and gate, wired through a shared hub.
struct Participant {
    atlas::sim::CommandQueue queue;
    atlas::sim::TurnGate gate;
    std::unique_ptr<atlas::net::Session> session;
    atlas::Tick next_turn = 0;
};

[[nodiscard]]  /// One message there and back over real sockets.
///
/// Both hubs are in this process, which is what makes the scenario runnable at all — a
/// benchmark cannot start a second process and still be a benchmark. It measures the host's
/// send-and-receive path and says nothing about a network.
[[nodiscard]] std::vector<Result> socket_results() {
    std::vector<Result> results;

    auto runtime = atlas::net::EnetRuntime::create();
    if (!runtime) {
        // Not fatal to the whole group. A machine with no loopback networking should still be
        // able to report the three scenarios above rather than abort.
        std::fprintf(stderr, "bench_net: no socket library, skipping the socket scenario\n");
        return results;
    }

    auto listener = atlas::net::EnetHub::listen(*runtime, 0, 2);
    if (!listener) {
        std::fprintf(stderr, "bench_net: could not listen, skipping the socket scenario\n");
        return results;
    }
    const std::uint16_t port = (*listener)->port();

    auto joined = std::async(std::launch::async, [&runtime, port] {
        return atlas::net::EnetHub::connect(*runtime, "127.0.0.1", port, std::chrono::seconds{10});
    });
    if (!(*listener)->accept(std::chrono::seconds{10})) {
        die("accept");
    }
    auto connector = joined.get();
    if (!connector) {
        die("connect");
    }

    const std::vector<std::byte> message(256, std::byte{0x3C});

    // Fewer iterations than the in-memory scenarios by two orders of magnitude, because each
    // one is a pair of system calls rather than a memcpy, and a benchmark that takes a minute
    // to say one number is a benchmark nobody runs.
    results.push_back(atlas::bench::measure("net/socket_roundtrip", "bytes=256", 2'000, 200, [&] {
        if (!(*listener)->send(0, 1, message)) {
            die("send");
        }
        // Pump both until it comes back, which is the round trip: the reply is sent by the
        // connector the moment it sees the request, so this times the whole path rather
        // than one direction with a guess about the other.
        auto& there = (*connector)->inbox(1, 0);
        auto& back = (*listener)->inbox(0, 1);
        std::vector<std::vector<std::byte>> drained;
        bool replied = false;
        for (int spin = 0; spin < 100'000; ++spin) {
            (*listener)->pump(0);
            (*connector)->pump(1);
            if (!replied && there.depth() > 0) {
                there.drain(drained);
                if (!(*connector)->send(1, 0, message)) {
                    die("reply");
                }
                replied = true;
            }
            if (back.depth() > 0) {
                back.drain(drained);
                return;
            }
        }
        die("the message never came back");
    }));

    return results;
}

std::vector<Result> run() {
    std::vector<Result> results;

    for (const std::size_t peers : {std::size_t{2}, std::size_t{4}}) {
        for (const std::size_t commands : {std::size_t{0}, std::size_t{8}}) {
            auto hub = atlas::net::LoopbackHub::create({.peer_count = peers});
            if (!hub) {
                die("no hub");
            }

            std::vector<std::unique_ptr<Participant>> table;
            table.reserve(peers);
            for (std::size_t i = 0; i < peers; ++i) {
                auto participant = std::make_unique<Participant>();
                require(participant->queue.register_handler(kPoke, poke_handler()), "handler");
                auto session = atlas::net::Session::create((*hub)->end(i), {});
                if (!session) {
                    die("no session");
                }
                participant->session = *std::move(session);
                table.push_back(std::move(participant));
            }

            // Settle the handshake outside the timed section: it happens once and is not what
            // this measures.
            for (int attempt = 0; attempt < 64; ++attempt) {
                for (auto& participant : table) {
                    const auto report =
                        participant->session->poll(0, participant->queue, participant->gate);
                    if (!report) {
                        die("handshake");
                    }
                }
            }

            // Deliberately no kernel step. Stepping would measure the simulation and report the
            // network as free.
            results.push_back(atlas::bench::measure(
                "net/gate_and_poll", std::format("peers={} commands={}", peers, commands), 2000,
                200, [&table, commands] {
                    for (auto& participant : table) {
                        const auto tick = participant->next_turn++;
                        auto turn = turn_of(commands, participant->session->self(), tick);
                        require(participant->session->send_turn(tick, turn, participant->gate),
                                "send_turn");
                    }
                    for (auto& participant : table) {
                        const auto report =
                            participant->session->poll(0, participant->queue, participant->gate);
                        if (!report) {
                            die("poll");
                        }
                        // Retired so the gate's window does not run out over two thousand
                        // iterations, which would turn this into a measurement of refusals.
                        participant->gate.retire_before(participant->gate.ready_horizon());
                        participant->queue.clear();
                    }
                }));
        }
    }

    for (const std::size_t commands : {std::size_t{0}, std::size_t{8}, std::size_t{64}}) {
        const atlas::net::Message message{
            atlas::net::Turn{.tick = 41,
                             .source = atlas::sim::SourceId{1},
                             .commands = turn_of(commands, atlas::sim::SourceId{1}, 41)}};
        results.push_back(atlas::bench::measure(
            "net/encode_turn", std::format("commands={}", commands), 20'000, 2'000, [&message] {
                const auto bytes = atlas::net::encode(message);
                if (!bytes) {
                    die("encode");
                }
            }));

        const auto encoded = atlas::net::encode(message);
        if (!encoded) {
            die("encode");
        }
        results.push_back(atlas::bench::measure(
            "net/decode_turn", std::format("commands={}", commands), 20'000, 2'000, [&encoded] {
                const auto back = atlas::net::decode(*encoded);
                if (!back) {
                    die("decode");
                }
            }));
    }

    for (auto& result : socket_results()) {
        results.push_back(std::move(result));
    }

    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("net", run);

}  // namespace

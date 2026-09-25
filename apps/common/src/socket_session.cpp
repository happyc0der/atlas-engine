// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/socket_session.hpp>
#include <atlas/core/log.hpp>

#include <charconv>
#include <cstdio>
#include <format>
#include <thread>
#include <utility>

namespace atlas::app {

Result<SocketEndpoint> parse_connect(std::string_view host_port) {
    const auto colon = host_port.rfind(':');
    if (colon == std::string_view::npos) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "--connect wants HOST:PORT"));
    }
    const std::string_view host = host_port.substr(0, colon);
    const std::string_view port_text = host_port.substr(colon + 1);
    if (host.empty()) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "--connect wants a host before the port"));
    }
    std::uint32_t port = 0;
    const auto* first = port_text.data();
    const auto* last = first + port_text.size();
    const auto [end, error] = std::from_chars(first, last, port);
    if (error != std::errc{} || end != last || port == 0 || port > 65535) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, std::format("'{}' is not a port", port_text)));
    }
    return SocketEndpoint{
        .listen = false, .host = std::string{host}, .port = static_cast<std::uint16_t>(port)};
}

Result<std::unique_ptr<net::EnetHub>> open_socket_hub(const net::EnetRuntime& runtime,
                                                      const SocketEndpoint& endpoint,
                                                      std::chrono::milliseconds timeout,
                                                      const net::EnetConfig& config) {
    if (endpoint.listen) {
        auto hub = net::EnetHub::listen(runtime, endpoint.port, endpoint.expected_peers, config);
        if (!hub) {
            return std::unexpected(std::move(hub).error().context("listening"));
        }
        std::printf("listening on port %u\n", static_cast<unsigned>((*hub)->port()));
        std::fflush(stdout);
        if (auto status = (*hub)->accept(timeout); !status) {
            return std::unexpected(std::move(status).error().context("waiting for peers"));
        }
        return hub;
    }
    auto hub = net::EnetHub::connect(runtime, endpoint.host, endpoint.port, timeout, config);
    if (!hub) {
        return std::unexpected(std::move(hub).error().context("connecting"));
    }
    return hub;
}

Status await_handshake(net::Session& session, const net::EnetHub& hub, sim::CommandQueue& queue,
                       sim::TurnGate& gate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!session.running()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return std::unexpected(Error(ErrorCode::Unavailable,
                                         "the handshake did not complete before the deadline"));
        }
        auto report = session.poll(0, queue, gate);
        if (!report) {
            return std::unexpected(std::move(report).error().context("the handshake"));
        }
        if (hub.status().ended) {
            return std::unexpected(
                Error(ErrorCode::Unavailable,
                      std::format("the link ended during the handshake: {}", hub.status().reason)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return {};
}

}  // namespace atlas::app

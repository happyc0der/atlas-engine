// SPDX-License-Identifier: GPL-3.0-or-later
//
// The socket transport, and the only file in this project that includes ENet.
//
// Everything here is one implementation of `net::Link`. Nothing above it knows a socket exists,
// which is what ADR-0017's rollback section means by "deleting one class": the loopback and the
// session are unaffected by anything in this file.

#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/net/enet_hub.hpp>

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace atlas::net {
namespace {

constexpr log::Category kNet{"net"};

/// One channel, reliable and ordered.
///
/// **This is the whole reason ENet was chosen over a raw datagram socket** (ADR-0017 D1):
/// lockstep tolerates no loss and no reordering, and one reliable ordered channel is exactly
/// the guarantee the message codec was written against. A second channel would reintroduce the
/// possibility of two messages arriving out of order relative to each other, which is the thing
/// `Turn` being one message rather than two exists to prevent.
constexpr enet_uint8 kChannel = 0;
constexpr std::size_t kChannelCount = 1;

/// The handshake message the listener sends, before ADR-0014's own `Hello` exchange.
///
/// Four bytes: a marker and the index assigned. Deliberately not a `net::Message` — those are
/// the session's, and the session cannot start until it knows which `SourceId` it is. This is
/// the one thing that has to happen before the protocol proper, so it is as small as it can be.
constexpr std::array<std::byte, 2> kIndexMarker{std::byte{'A'}, std::byte{'X'}};
constexpr std::size_t kIndexMessageBytes = 4;

/// At most one runtime, tracked here rather than hidden inside the hub (ADR-0017 D10).
///
/// A function-local static rather than a namespace-scope one, which is the shape
/// `script::Runtime` already uses for exactly this: no static initialisation order to get
/// wrong, and it is the form the project's static analysis accepts for the small amount of
/// process-wide state an RAII owner needs to police itself.
[[nodiscard]] std::atomic<bool>& runtime_alive() noexcept {
    static std::atomic<bool> alive{false};
    return alive;
}

}  // namespace

// ---------------------------------------------------------------------------- EnetRuntime

Result<EnetRuntime> EnetRuntime::create() {
    ATLAS_ASSERT_MSG(!runtime_alive().load(std::memory_order_acquire),
                     "a second EnetRuntime while one is alive");

    if (enet_initialize() != 0) {
        return std::unexpected(
            Error(ErrorCode::PlatformInitFailed, "could not initialise the socket library"));
    }

    runtime_alive().store(true, std::memory_order_release);
    EnetRuntime runtime;
    runtime.m_owns = true;
    return runtime;
}

EnetRuntime::~EnetRuntime() {
    if (m_owns) {
        enet_deinitialize();
        runtime_alive().store(false, std::memory_order_release);
    }
}

EnetRuntime::EnetRuntime(EnetRuntime&& other) noexcept
    : m_owns(std::exchange(other.m_owns, false)) {}

EnetRuntime& EnetRuntime::operator=(EnetRuntime&& other) noexcept {
    if (this != &other) {
        if (m_owns) {
            enet_deinitialize();
            runtime_alive().store(false, std::memory_order_release);
        }
        m_owns = std::exchange(other.m_owns, false);
    }
    return *this;
}

bool EnetRuntime::alive() noexcept {
    return runtime_alive().load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------- EnetHub

struct EnetHub::Impl {
    ENetHost* host = nullptr;
    EnetConfig config;

    std::size_t local = 0;
    std::uint16_t bound_port = 0;

    /// One entry per peer index. The local index's entry is null and never used, which costs a
    /// pointer and removes an index adjustment from every lookup — the same trade the loopback
    /// makes with its inboxes.
    std::vector<ENetPeer*> peers;

    /// `[receiver][sender]`, row-major, matching the loopback's layout so a reader moving
    /// between the two is not re-learning an indexing scheme.
    std::vector<std::unique_ptr<CommandInbox>> inboxes;

    EnetStatus status;
    std::chrono::steady_clock::time_point last_heard;

    [[nodiscard]] std::size_t index_of(const ENetPeer* peer) const {
        for (std::size_t i = 0; i < peers.size(); ++i) {
            if (peers[i] == peer) {
                return i;
            }
        }
        return peers.size();
    }

    /// Set up the inbox grid once the peer count is known.
    void make_inboxes(std::size_t count) {
        peers.assign(count, nullptr);
        inboxes.resize(count * count);
        for (auto& box : inboxes) {
            box = std::make_unique<CommandInbox>();
        }
        last_heard = std::chrono::steady_clock::now();
    }

    void end(std::string reason) {
        if (status.ended) {
            return;
        }
        status.ended = true;
        status.reason = std::move(reason);
        ATLAS_LOG_ERROR(kNet, "session ended: {}", status.reason);
    }
};

EnetHub::EnetHub() : m_impl(std::make_unique<Impl>()) {}

EnetHub::~EnetHub() {
    if (!m_impl || m_impl->host == nullptr) {
        return;
    }

    // **Say goodbye before closing, rather than just closing.** Destroying the host drops the
    // socket without telling anyone, so every peer would sit waiting for a timeout to decide
    // something it could have been told immediately — ten seconds of a session that is already
    // over. `_now` rather than the graceful form because a destructor must not wait: this
    // queues the notification and sends it in the same call.
    //
    // A process that is killed outright cannot do this, which is exactly why the timeout in
    // `pump` exists as well. The two cover different failures and neither replaces the other.
    for (ENetPeer* peer : m_impl->peers) {
        if (peer != nullptr) {
            enet_peer_disconnect_now(peer, 0);
        }
    }
    enet_host_flush(m_impl->host);
    enet_host_destroy(m_impl->host);
}

std::size_t EnetHub::peer_count() const noexcept {
    return m_impl->peers.size();
}

std::size_t EnetHub::local_index() const noexcept {
    return m_impl->local;
}

std::uint16_t EnetHub::port() const noexcept {
    return m_impl->bound_port;
}

const EnetStatus& EnetHub::status() const noexcept {
    return m_impl->status;
}

CommandInbox& EnetHub::inbox(std::size_t peer, std::size_t from) {
    return *m_impl->inboxes[(peer * m_impl->peers.size()) + from];
}

Status EnetHub::send(std::size_t from, std::size_t to, std::span<const std::byte> message) {
    if (from != m_impl->local) {
        // A hub speaks only for the process it is in. The loopback can carry any pair because
        // every peer is in one process; here, claiming to be another peer is not a thing a
        // socket could do even if the caller asked.
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "a socket hub sends only as its own peer"));
    }
    if (to >= m_impl->peers.size() || to == m_impl->local) {
        return std::unexpected(Error(ErrorCode::OutOfRange, "no such peer, or sending to oneself"));
    }
    if (m_impl->status.ended) {
        return std::unexpected(Error(ErrorCode::Unavailable, m_impl->status.reason));
    }
    if (message.size() > m_impl->config.max_message_bytes) {
        return std::unexpected(Error(
            ErrorCode::OutOfRange, std::format("a message of {} bytes is past the limit of {}",
                                               message.size(), m_impl->config.max_message_bytes)));
    }

    ENetPeer* peer = m_impl->peers[to];
    if (peer == nullptr) {
        return std::unexpected(Error(ErrorCode::Unavailable, "that peer has gone"));
    }

    ENetPacket* packet =
        enet_packet_create(message.data(), message.size(), ENET_PACKET_FLAG_RELIABLE);
    if (packet == nullptr) {
        return std::unexpected(Error(ErrorCode::Exhausted, "could not allocate a packet"));
    }
    if (enet_peer_send(peer, kChannel, packet) != 0) {
        // Ownership passes to ENet on success only, so this is the one path that must destroy
        // it. Leaking here would be invisible until a session had run for a long time.
        enet_packet_destroy(packet);
        return std::unexpected(Error(ErrorCode::Unavailable, "could not queue the message"));
    }
    return {};
}

void EnetHub::pump(std::size_t peer) {
    ATLAS_ASSERT_MSG(peer == m_impl->local, "a socket hub pumps only its own end");
    if (m_impl->status.ended) {
        return;
    }

    // Zero timeout: this drains what has already arrived and returns. ADR-0017 D3 chose a
    // polled transport, so this runs inside the frame that was happening anyway and must not
    // wait for anything.
    ENetEvent event{};
    while (enet_host_service(m_impl->host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
            const std::size_t from = m_impl->index_of(event.peer);
            if (from < m_impl->peers.size()) {
                // The one place this file crosses from the library's bytes to ours. ENet hands
                // back `enet_uint8*`; `as_bytes` is the permitted direction and there is no
                // inverse, so the cast is unavoidable and its extent is the copy below.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                const auto* data = reinterpret_cast<const std::byte*>(event.packet->data);
                auto& box = inbox(m_impl->local, from);
                // Copied out of ENet's buffer, which is freed below. The inbox owns what it
                // holds precisely so the transport's allocation lifetime stops here.
                std::vector<std::byte> message(data, data + event.packet->dataLength);
                if (box.push(std::move(message)) == CommandInbox::Push::Overflowed) {
                    m_impl->end(std::format("peer {} overran its inbox", from));
                }
            }
            enet_packet_destroy(event.packet);
            m_impl->last_heard = std::chrono::steady_clock::now();
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const std::size_t gone = m_impl->index_of(event.peer);
            if (gone < m_impl->peers.size()) {
                m_impl->peers[gone] = nullptr;
                m_impl->status.closed.push_back(gone);
            }
            // **Ends the session rather than dropping the peer** (ADR-0017 D5). Continuing is
            // simulation-visible: every remaining peer would have to apply the drop at the
            // identical tick or diverge, which needs an agreement protocol this milestone
            // deliberately does not build.
            m_impl->end(std::format("peer {} disconnected", gone));
            break;
        }
        case ENET_EVENT_TYPE_CONNECT:
            // Nobody joins a session that has started. The expectation set was fixed before the
            // first tick and lockstep never re-opens it, so this is a stranger and is refused.
            ATLAS_LOG_WARN(kNet, "refusing a connection to a session already under way");
            enet_peer_disconnect_now(event.peer, 0);
            break;
        case ENET_EVENT_TYPE_NONE: break;
        }
        event = ENetEvent{};
    }

    // The one deadline in the design, and it is here rather than in the turn gate, so the gate
    // still decides readiness from who has reported and never from a clock (ADR-0014).
    const auto quiet = std::chrono::steady_clock::now() - m_impl->last_heard;
    if (!m_impl->status.ended && quiet > m_impl->config.peer_timeout) {
        m_impl->end(
            std::format("no peer has been heard from for {}ms",
                        std::chrono::duration_cast<std::chrono::milliseconds>(quiet).count()));
    }
}

namespace {

/// Pack "you are peer N" into four bytes.
[[nodiscard]] std::array<std::byte, kIndexMessageBytes> index_message(std::size_t index) {
    return {kIndexMarker[0], kIndexMarker[1], static_cast<std::byte>(index & 0xFFU),
            static_cast<std::byte>((index >> 8U) & 0xFFU)};
}

/// Read one back, refusing anything that is not exactly it.
///
/// Untrusted like every other input: a listener that sent something else, or a stray packet
/// from an unrelated program on the same port, must not be read as an index.
[[nodiscard]] Result<std::size_t> read_index_message(const ENetPacket& packet) {
    if (packet.dataLength != kIndexMessageBytes) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "the listener's first message was the wrong size"));
    }
    if (static_cast<std::byte>(packet.data[0]) != kIndexMarker[0] ||
        static_cast<std::byte>(packet.data[1]) != kIndexMarker[1]) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "the listener's first message was not one of ours"));
    }
    const auto low = static_cast<std::size_t>(packet.data[2]);
    const auto high = static_cast<std::size_t>(packet.data[3]);
    const std::size_t index = low | (high << 8U);
    if (index == 0 || index >= kMaxPeers) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange, std::format("the listener assigned index {}", index)));
    }
    return index;
}

}  // namespace

Result<std::unique_ptr<EnetHub>> EnetHub::listen(const EnetRuntime& runtime, std::uint16_t port,
                                                 std::size_t expected_peers,
                                                 const EnetConfig& config) {
    ATLAS_ASSERT_MAIN_THREAD();
    (void)runtime;  // Held by the caller for its lifetime; taken to make that requirement visible.

    if (expected_peers < 2 || expected_peers > kMaxPeers) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange, std::format("a session of {} peers is outside 2..{}",
                                                     expected_peers, kMaxPeers)));
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    auto hub = std::unique_ptr<EnetHub>(new EnetHub());
    hub->m_impl->config = config;
    hub->m_impl->local = 0;

    hub->m_impl->host = enet_host_create(&address, expected_peers - 1, kChannelCount, 0, 0);
    if (hub->m_impl->host == nullptr) {
        return std::unexpected(
            Error(ErrorCode::PlatformInitFailed, std::format("could not listen on port {}", port)));
    }
    hub->m_impl->bound_port = hub->m_impl->host->address.port;
    hub->m_impl->make_inboxes(expected_peers);

    ATLAS_LOG_INFO(kNet, "listening on port {} for {} peer(s)", hub->m_impl->bound_port,
                   expected_peers - 1);

    return hub;
}

Status EnetHub::accept(std::chrono::milliseconds timeout) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT_MSG(m_impl->local == 0, "only a listener accepts");

    const std::size_t expected_peers = m_impl->peers.size();

    // **The listener assigns every index, in connection order** (ADR-0017 D8). Somebody must,
    // and both ends need to agree before any SourceId is stamped, because a SourceId reaches
    // the replay and the hash and cannot be provisional.
    std::size_t next_index = 1;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (next_index < expected_peers) {
        if (std::chrono::steady_clock::now() > deadline) {
            return std::unexpected(
                Error(ErrorCode::Unavailable,
                      std::format("only {} of {} peer(s) connected before the deadline",
                                  next_index - 1, expected_peers - 1)));
        }

        ENetEvent event{};
        if (enet_host_service(m_impl->host, &event, 50) <= 0) {
            continue;
        }
        if (event.type != ENET_EVENT_TYPE_CONNECT) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
            continue;
        }

        const auto assigned = index_message(next_index);
        ENetPacket* packet =
            enet_packet_create(assigned.data(), assigned.size(), ENET_PACKET_FLAG_RELIABLE);
        if (packet == nullptr || enet_peer_send(event.peer, kChannel, packet) != 0) {
            if (packet != nullptr) {
                enet_packet_destroy(packet);
            }
            return std::unexpected(
                Error(ErrorCode::Unavailable, "could not tell a peer which index it is"));
        }
        enet_host_flush(m_impl->host);

        m_impl->peers[next_index] = event.peer;
        ATLAS_LOG_INFO(kNet, "peer {} connected", next_index);
        ++next_index;
    }

    m_impl->last_heard = std::chrono::steady_clock::now();
    return {};
}

Result<std::unique_ptr<EnetHub>> EnetHub::connect(const EnetRuntime& runtime, std::string_view host,
                                                  std::uint16_t port,
                                                  std::chrono::milliseconds timeout,
                                                  const EnetConfig& config) {
    // **No main-thread assertion here, and that is a decision rather than an omission.**
    // `connect` opens a socket and hands back a hub nobody else holds yet; it touches no state
    // this process shares, exactly as an asset importer decodes on a worker and hands back
    // bytes. Every call that touches a *live* hub — `accept`, `pump`, `send`, `inbox` — asserts,
    // because those are the ones where two threads would be a race rather than a handoff.
    //
    // The concrete thing this buys: a test can bring both halves of a connection up in one
    // process, and a future application can connect without blocking the frame it is drawing.
    (void)runtime;

    ENetAddress address{};
    const std::string host_text{host};
    if (enet_address_set_host(&address, host_text.c_str()) != 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, std::format("could not resolve '{}'", host_text)));
    }
    address.port = port;

    auto hub = std::unique_ptr<EnetHub>(new EnetHub());
    hub->m_impl->config = config;

    hub->m_impl->host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (hub->m_impl->host == nullptr) {
        return std::unexpected(Error(ErrorCode::PlatformInitFailed, "could not open a socket"));
    }

    ENetPeer* listener = enet_host_connect(hub->m_impl->host, &address, kChannelCount, 0);
    if (listener == nullptr) {
        return std::unexpected(
            Error(ErrorCode::Unavailable, std::format("no route to {}:{}", host_text, port)));
    }

    // Wait for the connection and then for the index, which arrive as two events. Until the
    // second one this process does not know what it is, so there is nothing it could usefully
    // do in the meantime.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool connected = false;
    while (std::chrono::steady_clock::now() <= deadline) {
        ENetEvent event{};
        if (enet_host_service(hub->m_impl->host, &event, 50) <= 0) {
            continue;
        }
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            connected = true;
            continue;
        }
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            return std::unexpected(
                Error(ErrorCode::Unavailable, "the listener closed the connection"));
        }
        if (event.type != ENET_EVENT_TYPE_RECEIVE) {
            continue;
        }

        const auto index = read_index_message(*event.packet);
        enet_packet_destroy(event.packet);
        if (!index) {
            return std::unexpected(index.error());
        }

        // The listener is peer zero by construction, so a connector knows the whole shape from
        // its own index alone: it and everyone below it exist.
        hub->m_impl->local = *index;
        hub->m_impl->make_inboxes(*index + 1);
        hub->m_impl->peers[0] = listener;
        hub->m_impl->bound_port = hub->m_impl->host->address.port;
        ATLAS_LOG_INFO(kNet, "connected to {}:{} as peer {}", host_text, port, *index);
        return hub;
    }

    return std::unexpected(Error(
        ErrorCode::Unavailable, connected ? "connected, but was never told which peer it is"
                                          : std::format("could not reach {}:{}", host_text, port)));
}

}  // namespace atlas::net
